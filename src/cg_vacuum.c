/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc.
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
 *
 * $Id: cg_vacuum.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains code used to implement the VACUUM command.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

#if !defined(DBSQL_OMIT_VACUUM) || DBSQL_OMIT_VACUUM

/*
 * __vacuum --
 *	The non-standard VACUUM command is used to clean up the database,
 *	collapse free space, etc.  It is modelled after the VACUUM command
 *	in PostgreSQL.
 *
 * PUBLIC: void __vacuum __P((parser_t *, token_t *));
 */
void
__vacuum(parser, tab_name)
	parser_t *parser;
	token_t *tab_name;
{
	vdbe_t *v = __parser_get_vdbe(parser);
	__vdbe_add_op(v, OP_Vacuum, 0, 0);
	return;
}

/*
 * __execute_vacuum --
 *	This routine implements the OP_Vacuum opcode of the VDBE.  It works
 *	by running through each database asking each to reclaim space.
 *
 * PUBLIC: int __execute_vacuum __P((char **, DBSQL *));
 */
int
__execute_vacuum(err_msgs, dbp)
	char **err_msgs;
	DBSQL *dbp;
{
	return DBSQL_SUCCESS; /* NOTE: When DB implements compaction (someday)
				 then this will be the place to invoke the
				 __sm_compact() function.  For now, its
				 a no-op. */
}

#else

/*
 * __execute_vacuum --
 *	A no-op.
 *
 * PUBLIC: int __execute_vacuum __P((char **, DBSQL *));
 */
int
__execute_vacuum(err_msgs, dbp)
	char **err_msgs;
	DBSQL *dbp;
{
	return DBSQL_SUCCESS;
}

#endif
