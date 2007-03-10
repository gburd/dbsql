/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * $Id: safety.c 7 2007-02-03 13:34:17Z gburd $
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __safety_on --
 *	Change the sqlite.magic from DBSQL_STATUS_OPEN to DBSQL_STATUS_BUSY.
 *	Return an error (non-zero) if the magic was not DBSQL_STATUS_OPEN
 *	when this routine is called.
 *
 *	!!! TODO race condition
 *	This routine is a attempt to detect if two threads use the
 *	same DBSQL* pointer at the same time.  There is a race 
 *	condition so it is possible that the error is not detected.
 *	But usually the problem will be seen.  The result will be an
 *	error which can be used to debug the application that is
 *	using the library incorrectly.
 *
 *	[#202sl]:  If db->magic is not a valid open value, take care not
 *	to modify the db structure at all.  It could be that db is a stale
 *	pointer.  In other words, it could be that there has been a prior
 *	call to DBSQL->close(db) and db has been deallocated.  And we do
 *	not want to write into deallocated memory.
 *
 * PUBLIC: int __safety_on __P((DBSQL *));
 */
int
__safety_on(dbp)
	DBSQL *dbp;
{
	if (dbp->magic == DBSQL_STATUS_OPEN) {
		dbp->magic = DBSQL_STATUS_BUSY;
		return 0;
	} else if (dbp->magic == DBSQL_STATUS_BUSY ||
		   dbp->magic == DBSQL_STATUS_ERROR ||
		   dbp->want_to_close) {
		dbp->magic = DBSQL_STATUS_ERROR;
		dbp->flags |= DBSQL_Interrupt;
	}
	return 1;
}

/*
 * __safety_off --
 *	Change the magic from DBSQL_STATUS_BUSY to DBSQL_STATUS_OPEN.
 *	Return an error (non-zero) if the magic was not DBSQL_STATUS_BUSY
 *	when this routine is called.
 *
 * PUBLIC: int __safety_off __P((DBSQL *));
 */
int
__safety_off(dbp)
	DBSQL *dbp;
{
	if (dbp->magic == DBSQL_STATUS_BUSY) {
		dbp->magic = DBSQL_STATUS_OPEN;
		return 0;
	} else if (dbp->magic == DBSQL_STATUS_OPEN ||
		   dbp->magic == DBSQL_STATUS_ERROR ||
		   dbp->want_to_close) {
		dbp->magic = DBSQL_STATUS_ERROR;
		dbp->flags |= DBSQL_Interrupt;
	}
	return 1;
}

/*
 * __safety_check --
 *	Check to make sure we are not currently executing an DBSQL->exec().
 *	If we are currently in an DBSQL->exec(), return true and set
 *	dbsql_t.magic to DBSQL_STATUS_ERROR.  This will cause a complete
 *	shutdown of the database.  This routine is used to try to detect
 *	when API routines are called at the wrong time or in the wrong
 *	sequence.
 *
 * PUBLIC: int __safety_check __P((DBSQL *));
 */
int
__safety_check(dbp)
	DBSQL *dbp;
{
	if (dbp->pVdbe != 0) {
		dbp->magic = DBSQL_STATUS_ERROR;
		return 1;
	}
	return 0;
}
