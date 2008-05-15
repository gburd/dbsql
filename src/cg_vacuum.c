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
