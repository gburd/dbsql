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
__safety_on(DBSQL* dbp)
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
__safety_off(DBSQL* dbp)
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
__safety_check(DBSQL* dbp)
{
	// TODO: Check VDBE list - need dbp->pVdbe field
	// if (dbp->pVdbe != 0) {
	// 	dbp->magic = DBSQL_STATUS_ERROR;
	// 	return 1;
	// }
	return 0;
}
