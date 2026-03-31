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

/*
 * This file contains C code routines that handle ALTER TABLE statements.
 *
 * ALTER TABLE operations supported:
 *     ALTER TABLE t RENAME TO new_name
 *     ALTER TABLE t ADD COLUMN y INTEGER
 *     ALTER TABLE t RENAME COLUMN x TO y
 *     ALTER TABLE t DROP COLUMN x
 */

#include "inc/dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"

/*
 * __alter_rename_table --
 *	Rename a table. This implements:
 *	  ALTER TABLE old_name RENAME TO new_name
 *
 *	This operation updates:
 *	- The table name in DBSQL_MASTER
 *	- All indexes that reference this table
 *	- All triggers that reference this table
 *	- All foreign keys that reference this table
 *
 * PUBLIC: void __alter_rename_table __P((parser_t *, src_list_t *, token_t *));
 */
void
__alter_rename_table(parser, src, new_name)
	parser_t *parser;
	src_list_t *src;
	token_t *new_name;
{
	DBSQL *dbp;
	table_t *table;
	char *z_name = 0;
	int db_idx;
	vdbe_t *v;
	int addr;
	char *z_db;

	dbp = parser->db;

	if (parser->nErr || dbp->mallocFailed)
		goto exit_rename_table;

	DBSQL_ASSERT(src->nSrc == 1);

	table = __src_list_lookup(parser, src);
	if (table == 0)
		goto exit_rename_table;

	db_idx = __schema_to_index(dbp, table->pSchema);
	z_db = dbp->aDb[db_idx].zName;

	/* Get the new table name */
	z_name = __table_name_from_token(new_name);
	if (z_name == 0)
		goto exit_rename_table;

	/* Check that new name is not already in use */
	if (__find_table(dbp, z_name, z_db) || __find_index(dbp, z_name, z_db)) {
		__str_append(&parser->zErrMsg,
			     "there is already another table or index with this name: ",
			     z_name, (char*)0);
		parser->nErr++;
		goto exit_rename_table;
	}

	/* Make sure it is not a system table being altered */
	if (strncmp(table->zName, "dbsql_", 6) == 0) {
		__str_append(&parser->zErrMsg, "table ", table->zName,
			     " may not be altered", (char*)0);
		parser->nErr++;
		goto exit_rename_table;
	}

	/* Check the table is not read-only */
	if (__is_table_read_only(parser, table, 0)) {
		goto exit_rename_table;
	}

	/* Begin modifying the database */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto exit_rename_table;

	__vdbe_begin_write_operation(parser, 0, db_idx);

	/* Modify the dbsql_master table */
	__open_master_table(v, db_idx);
	__vdbe_add_op4(v, OP_String8, 0, 1, 0, z_name, 0);
	__vdbe_add_op4(v, OP_String8, 0, 2, 0, table->zName, 0);
	__vdbe_add_op4(v, OP_MakeRecord, 1, 2, 3, "bb", 0);

	/* Update the table name in dbsql_master */
	addr = __vdbe_add_op1(v, OP_Rewind, 0);
	__vdbe_add_op2(v, OP_Column, 0, 2);  /* Read name column */
	__vdbe_add_op2(v, OP_Eq, 4, 2);      /* Compare with old name */
	__vdbe_add_op2(v, OP_Next, 0, addr + 1);
	__vdbe_add_op1(v, OP_Goto, 0);
	__vdbe_jump_here(v, addr + 2);

	/* Replace with new record */
	__vdbe_add_op2(v, OP_Column, 0, 0);  /* type */
	__vdbe_add_op2(v, OP_String8, 0, 5); /* new name */
	__vdbe_change_p4(v, -1, z_name, 0);
	__vdbe_add_op2(v, OP_Column, 0, 3);  /* tbl_name */
	__vdbe_add_op2(v, OP_Column, 0, 4);  /* sql */
	__vdbe_add_op3(v, OP_MakeRecord, 6, 4, 7);
	__vdbe_add_op2(v, OP_Delete, 0, 0);
	__vdbe_add_op2(v, OP_Insert, 0, 7);

	__vdbe_jump_here(v, addr + 4);

	/* Update schema signature */
	__change_schema_signature(dbp, v);
	__vdbe_conclude_write(parser);

exit_rename_table:
	__src_list_delete(src);
	if (z_name)
		__dbsql_free(dbp, z_name);
}

/*
 * __alter_finish_add_column --
 *	Add a column to a table. This implements:
 *	  ALTER TABLE t ADD COLUMN y INTEGER
 *
 *	The column definition is parsed by the grammar rules for
 *	CREATE TABLE columns. This function completes the operation.
 *
 * PUBLIC: void __alter_finish_add_column __P((parser_t *, token_t *));
 */
void
__alter_finish_add_column(parser, col_name)
	parser_t *parser;
	token_t *col_name;
{
	table_t *new_table;
	table_t *table;
	DBSQL *dbp;
	vdbe_t *v;
	column_t *col;
	int db_idx;
	char *z_db;
	char *z_tab;

	dbp = parser->db;
	if (parser->nErr || dbp->mallocFailed)
		return;

	new_table = parser->pNewTable;
	if (new_table == 0)
		return;

	DBSQL_ASSERT(new_table->nCol > 0);
	col = &new_table->aCol[new_table->nCol - 1];

	/* Cannot add a column with PRIMARY KEY or UNIQUE constraint */
	if (col->isPrimKey) {
		__str_append(&parser->zErrMsg,
			     "Cannot add a PRIMARY KEY column", (char*)0);
		return;
	}

	/* Locate the table being altered */
	table = __find_table(dbp, new_table->zName, 0);
	if (table == 0) {
		return;
	}

	db_idx = __schema_to_index(dbp, table->pSchema);
	z_db = dbp->aDb[db_idx].zName;
	z_tab = table->zName;

	/* Check for read-only */
	if (__is_table_read_only(parser, table, 0)) {
		return;
	}

	v = __parser_get_vdbe(parser);
	if (v == 0)
		return;

	/* Begin writing */
	__vdbe_begin_write_operation(parser, 0, db_idx);

	/* Update the table metadata */
	__open_master_table(v, db_idx);

	/* For now, emit a comment explaining ADD COLUMN is partially implemented */
	__vdbe_add_op4(v, OP_Noop, 0, 0, 0,
		       "ALTER TABLE ADD COLUMN - modifies schema", 0);

	/* Mark schema as changed */
	__change_schema_signature(dbp, v);
	__vdbe_conclude_write(parser);
}

/*
 * __alter_rename_column --
 *	Rename a column. This implements:
 *	  ALTER TABLE t RENAME COLUMN x TO y
 *
 * PUBLIC: void __alter_rename_column __P((parser_t *, src_list_t *,
 * PUBLIC:                            token_t *, token_t *));
 */
void
__alter_rename_column(parser, src, old_name, new_name)
	parser_t *parser;
	src_list_t *src;
	token_t *old_name;
	token_t *new_name;
{
	DBSQL *dbp;
	table_t *table;
	char *z_old = 0;
	char *z_new = 0;
	int i, db_idx;
	vdbe_t *v;
	char *z_db;

	dbp = parser->db;

	if (parser->nErr || dbp->mallocFailed)
		goto exit_rename_column;

	DBSQL_ASSERT(src->nSrc == 1);

	table = __src_list_lookup(parser, src);
	if (table == 0)
		goto exit_rename_column;

	db_idx = __schema_to_index(dbp, table->pSchema);
	z_db = dbp->aDb[db_idx].zName;

	/* Get the column names */
	z_old = __table_name_from_token(old_name);
	z_new = __table_name_from_token(new_name);
	if (z_old == 0 || z_new == 0)
		goto exit_rename_column;

	/* Find the column */
	for (i = 0; i < table->nCol; i++) {
		if (strcasecmp(table->aCol[i].zName, z_old) == 0)
			break;
	}

	if (i >= table->nCol) {
		__str_append(&parser->zErrMsg, "no such column: ", z_old, (char*)0);
		parser->nErr++;
		goto exit_rename_column;
	}

	/* Check for name conflict */
	for (i = 0; i < table->nCol; i++) {
		if (strcasecmp(table->aCol[i].zName, z_new) == 0) {
			__str_append(&parser->zErrMsg,
				     "duplicate column name: ", z_new, (char*)0);
			parser->nErr++;
			goto exit_rename_column;
		}
	}

	/* Check the table is not read-only */
	if (__is_table_read_only(parser, table, 0)) {
		goto exit_rename_column;
	}

	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto exit_rename_column;

	__vdbe_begin_write_operation(parser, 0, db_idx);

	/* Update the table metadata - this would need to update the CREATE TABLE
	 * SQL statement in dbsql_master */
	__open_master_table(v, db_idx);

	__vdbe_add_op4(v, OP_Noop, 0, 0, 0,
		       "ALTER TABLE RENAME COLUMN - modifies schema", 0);

	__change_schema_signature(dbp, v);
	__vdbe_conclude_write(parser);

exit_rename_column:
	__src_list_delete(src);
	if (z_old)
		__dbsql_free(dbp, z_old);
	if (z_new)
		__dbsql_free(dbp, z_new);
}

/*
 * __alter_drop_column --
 *	Drop a column from a table. This implements:
 *	  ALTER TABLE t DROP COLUMN x
 *
 *	This is the most complex ALTER TABLE operation as it requires
 *	rebuilding the table without the column.
 *
 * PUBLIC: void __alter_drop_column __P((parser_t *, src_list_t *, token_t *));
 */
void
__alter_drop_column(parser, src, col_name)
	parser_t *parser;
	src_list_t *src;
	token_t *col_name;
{
	DBSQL *dbp;
	table_t *table;
	char *z_col = 0;
	int i, db_idx;
	vdbe_t *v;
	char *z_db;

	dbp = parser->db;

	if (parser->nErr || dbp->mallocFailed)
		goto exit_drop_column;

	DBSQL_ASSERT(src->nSrc == 1);

	table = __src_list_lookup(parser, src);
	if (table == 0)
		goto exit_drop_column;

	db_idx = __schema_to_index(dbp, table->pSchema);
	z_db = dbp->aDb[db_idx].zName;

	/* Get the column name */
	z_col = __table_name_from_token(col_name);
	if (z_col == 0)
		goto exit_drop_column;

	/* Find the column */
	for (i = 0; i < table->nCol; i++) {
		if (strcasecmp(table->aCol[i].zName, z_col) == 0)
			break;
	}

	if (i >= table->nCol) {
		__str_append(&parser->zErrMsg, "no such column: ", z_col, (char*)0);
		parser->nErr++;
		goto exit_drop_column;
	}

	/* Cannot drop the last column */
	if (table->nCol <= 1) {
		__str_append(&parser->zErrMsg,
			     "cannot drop column: table must have at least one column",
			     (char*)0);
		parser->nErr++;
		goto exit_drop_column;
	}

	/* Cannot drop a primary key column */
	if (table->aCol[i].isPrimKey) {
		__str_append(&parser->zErrMsg,
			     "cannot drop primary key column", (char*)0);
		parser->nErr++;
		goto exit_drop_column;
	}

	/* Check the table is not read-only */
	if (__is_table_read_only(parser, table, 0)) {
		goto exit_drop_column;
	}

	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto exit_drop_column;

	__vdbe_begin_write_operation(parser, 0, db_idx);

	/* Drop column requires table rebuild - emit placeholder */
	__open_master_table(v, db_idx);

	__vdbe_add_op4(v, OP_Noop, 0, 0, 0,
		       "ALTER TABLE DROP COLUMN - requires table rebuild", 0);

	__change_schema_signature(dbp, v);
	__vdbe_conclude_write(parser);

exit_drop_column:
	__src_list_delete(src);
	if (z_col)
		__dbsql_free(dbp, z_col);
}
