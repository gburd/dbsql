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
 * This file contains code used to implement the COPY command.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __copy --
 *	The COPY command is for compatibility with PostgreSQL and specificially
 *	for the ability to read the output of pg_dump.  The format is as
 *	follows:
 *
 *	   COPY table FROM file [USING DELIMITERS string]
 *
 *	"table" is an existing table name.  We will read lines of code from
 *	file to fill this table with data.  File might be "stdin".  The
 *	optional delimiter string identifies the field separators.  The
 *	default is a tab.
 *
 * PUBLIC: void __copy __P((parser_t *, src_list_t *, token_t *,
 * PUBLIC:             token_t *, int));
 *
 * parser			The parser context
 * table_name			The name of the table into which we will insert
 * filename			The file from which to obtain information
 * delimiter			Use this as the field delimiter
 * on_error			What to do if a constraint fails
 */
void __copy(parser, table_name, filename, delimiter, on_error)
	parser_t *parser;
	src_list_t *table_name;
	token_t *filename;
	token_t *delimiter;
	int on_error;
{
	table_t *table;
	int i;
	vdbe_t *v;
	int addr, end;
	index_t *index;
	char *file = 0;
	const char *db_name;
	DBSQL *db = parser->db;

	if (parser->rc != DBSQL_SUCCESS)
		goto copy_cleanup;
	DBSQL_ASSERT(table_name->nSrc == 1);
	table = __src_list_lookup(parser, table_name);
	if (table==0 || __is_table_read_only(parser, table, 0))
		goto copy_cleanup;
	__dbsql_strndup(parser->db, filename->z, &file, filename->n);
	__str_unquote(file);
	DBSQL_ASSERT(table->iDb < db->nDb);
	db_name = db->aDb[table->iDb].zName;
	if (__auth_check(parser, DBSQL_INSERT, table->zName, 0, db_name) ||
	    __auth_check(parser, DBSQL_COPY, table->zName, file, db_name)) {
		goto copy_cleanup;
	}
	v = __parser_get_vdbe(parser);
	if (v) {
		__vdbe_prepare_write(parser, 1, table->iDb);
		addr = __vdbe_add_op(v, OP_FileOpen, 0, 0);
		__vdbe_change_p4(v, addr, filename->z, filename->n);
		__vdbe_dequote_p3(v, addr);
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenWrite, 0, table->tnum);
		__vdbe_change_p4(v, -1, table->zName, P3_STATIC);
		for(i = 1, index = table->pIndex; index;
		    index=index->pNext, i++) {
			DBSQL_ASSERT(index->iDb == 1 || index->iDb == table->iDb);
			__vdbe_add_op(v, OP_Integer, index->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, i, index->tnum);
			__vdbe_change_p4(v, -1, index->zName, P3_STATIC);
		}
		if (db->flags & DBSQL_CountRows) {
                        /* Initialize the row count */
			__vdbe_add_op(v, OP_Integer, 0, 0);
		}
		end = __vdbe_make_label(v);
		addr = __vdbe_add_op(v, OP_FileRead, table->nCol, end);
		if (delimiter) {
			__vdbe_change_p4(v, addr, delimiter->z, delimiter->n);
			__vdbe_dequote_p3(v, addr);
		} else {
			__vdbe_change_p4(v, addr, "\t", 1);
		}
		if (table->iPKey >= 0) {
			__vdbe_add_op(v, OP_FileColumn, table->iPKey, 0);
			__vdbe_add_op(v, OP_MustBeInt, 0, 0);
		} else {
			__vdbe_add_op(v, OP_NewRecno, 0, 0);
		}
		for(i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				/*
				 * The integer primary key column is filled
				 * with NULL since its value is always pulled
				 * from the record number.
				 */
				__vdbe_add_op(v, OP_String, 0, 0);
			} else {
				__vdbe_add_op(v, OP_FileColumn, i, 0);
			}
		}
		__generate_constraint_checks(parser, table, 0, 0,
					     (table->iPKey >= 0), 0, on_error,
					     addr);
		__complete_insertion(parser, table, 0, 0, 0, 0, -1);
		if ((db->flags & DBSQL_CountRows) != 0) {
                        /* Increment row count */
			__vdbe_add_op(v, OP_AddImm, 1, 0);
		}
		__vdbe_add_op(v, OP_Goto, 0, addr);
		__vdbe_resolve_label(v, end);
		__vdbe_add_op(v, OP_Noop, 0, 0);
		__vdbe_conclude_write(parser);
		if (db->flags & DBSQL_CountRows) {
			__vdbe_add_op(v, OP_ColumnName, 0, 0);
			__vdbe_change_p4(v, -1, "rows inserted", P3_STATIC);
			__vdbe_add_op(v, OP_Callback, 1, 0);
		}
	}
  
copy_cleanup:
	__src_list_delete(table_name);
	__dbsql_free(parser->db, file);
	return;
}
