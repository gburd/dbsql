/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2026  The DBSQL Group, Inc.
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

/*
 * sm_int.h --
 *	Private definitions of the libdb storage manager.  The public seam
 *	is src/inc/sm.h (SQLite's btree.h, transformed); this header gives
 *	the concrete layout of the types sm.h only forward-declares.  See
 *	.agent/steering/STORAGE.md for the design.
 */

#ifndef	_SM_INT_H_
#define	_SM_INT_H_

#include <db.h>

/* Name of the per-file metadata sub-database and its format record. */
#define	SM_META_DB		"__dbsql_meta"
#define	SM_FORMAT_KEY		"format"
#define	SM_FORMAT_MAGIC		"dbsql 1"
/* Meta slot for the auto_vacuum mode (upstream's 16 slots leave 9-14
 * unused; upstream keeps the flag in header fields we do not have). */
#define	SM_META_AUTOVAC		13
/* Meta slot counting committed write transactions (PRAGMA data_version). */
#define	SM_META_COMMITS		14
/*
 * Cursor key/data buffers: initial size, and readable zero bytes kept
 * past every item (the VDBE may overread a short or corrupt record by a
 * few bytes, as upstream's page buffers allow).
 */
#define	SM_DBT_INIT		256
#define	SM_DBT_PAD		64
/* __sm_row_count_est counts at most this many entries. */
#define	SM_ROW_EST_MAX		100000

/* Sub-database name for SQL table/index number N: "t<N>". */
#define	SM_SUBDB_FMT		"t%u"
#define	SM_SUBDB_MAX		24

/* Bytes in an encoded rowid key (big-endian, sign bit flipped). */
#define	SM_ROWID_KEYLEN		8

/* Savepoint depth (SQLite allows unbounded; libdb nests child txns). */
#define	SM_TXN_INITIAL		16

/* Most payload __sm_payload_fetch exposes directly (upstream's page). */
#define	SM_PAYLOAD_LOCAL_MAX	65536

/*
 * sm_env_t --
 *	One libdb environment, shared process-wide by every connection that
 *	opens a database file in the same directory (persistent), or owned
 *	by one connection (private: :memory:, temp, ephemeral).
 */
typedef struct sm_env {
	DB_ENV *dbenv;
	char *home;			/* Directory; a private temp env's own
					   directory (removed on close);
					   NULL for other private envs. */
	int refs;			/* Connections/handles using it. */
	u_int32_t flags;
#define	SM_ENV_PRIVATE	0x01		/* DB_PRIVATE, connection-owned. */
#define	SM_ENV_NOTXN	0x02		/* mpool only (ephemeral tables). */
	char errmsg[256];		/* Last libdb error message. */
	struct sm_env *next;		/* Registry list (persistent only). */
} sm_env_t;

/* Cache sizes: shared per directory; private per connection. */
#define	SM_ENV_CACHE		(32 * 1024 * 1024)
#define	SM_PRIVATE_CACHE	(8 * 1024 * 1024)
#define	SM_MEMLOG_SIZE		(64 * 1024 * 1024)

/* __sm_env_open_private kinds. */
#define	SM_PRIV_EPHEMERAL	0
#define	SM_PRIV_MEMORY		1
#define	SM_PRIV_TEMP		2
#define	SM_TEMP_FILE		"temp.db"	/* In a temp env's home. */

/* Lock and log region sizing (see __sm_env_size_regions). */
#define	SM_LOCKS_INIT		10000
#define	SM_LOCKS_MAX		1000000
#define	SM_LOG_REGIONMAX	(4 * 1024 * 1024)
#define	SM_ENV_REGION_MAX	(512 * 1024 * 1024)
/* Checkpoint after a write commit once this much log (KB) has grown. */
#define	SM_CKPT_KBYTES		256
/* Transactions: one per page the shared cache can hold (see sm_env.c);
 * the smallest page size dbsql creates is 512 bytes, plus 25%. */
#define	SM_TX_MAX		(SM_ENV_CACHE / 512 + SM_ENV_CACHE / 2048)

/*
 * sm_dbh_t --
 *	A cached, open libdb DB handle for one sub-database (one SQL table
 *	or index) of one sm_shared_t.  DB->app_private points back here so
 *	the comparator can find key_info.
 */
typedef struct sm_dbh {
	DB *dbp;
	u_int32_t pgno;			/* SQL root page number. */
	int is_intkey;			/* Rowid table (BTREE_INTKEY). */
	key_info_t *key_info;		/* Referenced; NULL for rowid tables. */
	int level;			/* Txn stack level that opened it. */
	struct sm_dbh *next;
} sm_dbh_t;

/*
 * sm_txn_t --
 *	The transaction state of one connection in one environment.  All
 *	sm_t handles of a connection that live in the same environment share
 *	one sm_txn_t, so a transaction spanning main and same-directory
 *	attachments commits atomically.
 *
 *	stack[0] is the top-level txn (DB_TXN_SNAPSHOT for persistent envs).
 *	stack[1] is the level-0 content child; every SQL write goes to
 *	stack[depth - 1].  Savepoint k (SQLite numbering, 0-based) is the
 *	child at stack[k + 2].
 */
typedef struct sm_txn {
	sm_env_t *env;
	DBSQL *db;			/* Owning connection. */
	DB_TXN **stack;
	int depth;			/* Entries used in stack[]. */
	int alloc;			/* Entries allocated. */
	int state;			/* DBSQL_TXN_NONE / _READ / _WRITE. */
	int refs;			/* sm_t handles sharing this txn. */
	int committing;			/* Phase two already done this txn. */
	u_int32_t commit_flags;		/* DB_TXN_NOSYNC etc. (synchronous). */
	struct sm_txn *next;		/* Per-connection list. */
} sm_txn_t;

/*
 * sm_shared_t --
 *	One open database file (or private in-memory database) as seen by
 *	one connection.  dbsql is built with DBSQL_OMIT_SHARED_CACHE, so
 *	there is exactly one sm_t per sm_shared_t.
 */
struct sm_shared_t {
	DBSQL *db;			/* Owning connection. */
	sm_env_t *env;
	sm_txn_t *txn;			/* NULL for ephemeral (non-txn) dbs. */
	char *filename;			/* As given to __sm_open; "" if none. */
	char *uri_name;			/* filename in pager layout (4 NUL prefix,
					 * then name\0journal\0wal\0\0) for
					 * __database_name()/dbsql_uri_*; NULL until
					 * first __sm_pager_filename() call. */
	char *path;			/* libdb file name, NULL if in-memory. */
	char lock_obj[512];		/* Write-lock object: path + "\0wr". */
	u_int32_t lock_obj_len;
	DB *meta;			/* __dbsql_meta sub-database. */
	int meta_level;			/* Txn level that opened meta. */
	sm_dbh_t *handles;		/* Open sub-database handles. */
	u_int32_t page_size;		/* For newly created sub-databases. */
	int page_size_fixed;
	pgno_t max_page_count;
	int secure_delete;
	int n_backup;			/* Backups in progress reading this. */
	int snapshot_stale;		/* Read snapshot older than a commit. */
	int foreign;			/* Not a libdb file: DBSQL_NOTADB. */
	int wrote;			/* Committed a write since checkpoint. */
	int eph_kind[2];		/* Ephemeral: table kinds by pgno&1. */
	int cache_size;			/* PRAGMA cache_size (stored). */
	int auto_vacuum;		/* PRAGMA auto_vacuum (stored). */
	u_int32_t pager_flags;		/* PRAGMA synchronous etc. */
	void *schema;			/* __sm_schema blob. */
	void (*free_schema)(void *);
	int in_trans;			/* DBSQL_TXN_* for this database. */
	u_int32_t own_seen;		/* Our commits (data_version excludes). */
	u_int32_t commits_seen;		/* SM_META_COMMITS at our last txn. */
	u_int32_t commits_base;		/* ... at our first txn. */
	int commits_base_set;
	int read_only;
	u_int32_t flags;
#define	SM_EPHEMERAL	0x01		/* BTREE_SINGLE|BTREE_OMIT_JOURNAL. */
#define	SM_MEMORY	0x02		/* :memory: or temp database. */
#define	SM_WRLOCKED	0x04		/* Holds the database write lock. */
	struct sm_cursor_t *cursors;	/* Open cursors (linked by next). */
};

/*
 * sm_t --
 *	A connection's handle on one database (DBSQL.dbs[i].bt).
 */
struct sm_t {
	DBSQL *db;
	sm_shared_t *bt;
	u_int8_t in_trans;		/* DBSQL_TXN_* (mirrors bt->in_trans). */
	u_int32_t data_version_base;
};

/*
 * sm_cursor_t --
 *	A cursor on one sub-database.  key/data hold the current entry in
 *	buffers owned by the cursor (DB_DBT_REALLOC); payload pointers
 *	returned to the VDBE stay valid until the cursor moves.
 */
struct sm_cursor_t {
	u_int8_t state;			/* CURSOR_*; MUST be first (has_moved). */
	sm_t *btree;
	sm_shared_t *bt;
	sm_dbh_t *dbh;
	DBC *dbc;			/* NULL when closed (saved position). */
	DB_TXN *dbc_txn;		/* Txn the DBC was opened in. */
	key_info_t *key_info;		/* As passed to __sm_cursor. */
	u_int32_t pgno;
	int wr_flag;
	u_int8_t cur_flags;
	u_int8_t hints;
	int skip_next;			/* CURSOR_SKIPNEXT: next/prev no-op. */
	int fault;			/* Error code when state==CURSOR_FAULT. */
	int saved;			/* key/data hold a position to restore. */
	int at_eof;			/* Positioned past the last entry. */
	int empty;			/* Table of a not yet created db. */
	int pinned;			/* OP_CursorLock: may not be moved. */
	int incrblob;			/* A blob handle's cursor. */
	u_int64_t generation;		/* Bumped on each move (__sm_offset). */
	void *preformat;		/* __sm_transfer_row payload. */
	u_int32_t preformat_size;
	u_int32_t preformat_alloc;
	u_int8_t rowid_key[SM_ROWID_KEYLEN];
	DBT key;			/* Current key (rowid key or record). */
	DBT data;			/* Current data (row record or empty). */
	int64_t rowid;			/* Decoded key of a rowid cursor. */
	struct sm_cursor_t *next;	/* sm_shared_t.cursors list. */
};

/* Cursor states, as upstream btreeInt.h. */
#define	CURSOR_VALID		0
#define	CURSOR_INVALID		1
#define	CURSOR_SKIPNEXT		2
#define	CURSOR_REQUIRESEEK	3
#define	CURSOR_FAULT		4

#include "sm_ext.h"

#endif /* !_SM_INT_H_ */
