/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  DBSQL Group, Inc - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: api.c 7 2007-02-03 13:34:17Z gburd $
 */

/* util.c - various utility functions
 *
 * Copyright (C) 2005-2006 Gerhard Häring <gh@ghaering.de>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "module.h"
#include "connection.h"

int _dbsql_step_with_busyhandler(dbsql_stmt_t* statement, pydbsql_Connection* connection)
{
    int rc;

    Py_BEGIN_ALLOW_THREADS
    rc = dbsql_step(statement);
    Py_END_ALLOW_THREADS

    return rc;
}

/**
 * Checks the DBSQL error code and sets the appropriate DB-API exception.
 * Returns the error code (0 means no error occurred).
 */
int _pydbsql_seterror(int errorcode)
{

    switch (errorcode)
    {
        case DBSQL_SUCCESS:
            PyErr_Clear();
            break;
        case DBSQL_INTERNAL:
        case DBSQL_NOTFOUND:
            PyErr_SetString(pydbsql_InternalError, dbsql_strerror(errorcode));
            break;
        case DBSQL_NOMEM:
            (void)PyErr_NoMemory();
            break;
        case DBSQL_ERROR:
        case DBSQL_PERM:
        case DBSQL_ABORT:
        case DBSQL_BUSY:
        case DBSQL_LOCKED:
        case DBSQL_READONLY:
        case DBSQL_INTERRUPT:
        case DBSQL_IOERR:
        case DBSQL_FULL:
        case DBSQL_CANTOPEN:
        case DBSQL_PROTOCOL:
        case DBSQL_EMPTY:
        case DBSQL_SCHEMA:
            PyErr_SetString(pydbsql_OperationalError,
			    dbsql_strerror(errorcode));
            break;
        case DBSQL_CORRUPT:
            PyErr_SetString(pydbsql_DatabaseError, dbsql_strerror(errorcode));
            break;
        case DBSQL_TOOBIG:
            PyErr_SetString(pydbsql_DataError, dbsql_strerror(errorcode));
            break;
        case DBSQL_CONSTRAINT:
        case DBSQL_MISMATCH:
            PyErr_SetString(pydbsql_IntegrityError, dbsql_strerror(errorcode));
            break;
        case DBSQL_MISUSE:
            PyErr_SetString(pydbsql_ProgrammingError,
			    dbsql_strerror(errorcode));
            break;
        default:
            PyErr_SetString(pydbsql_DatabaseError, dbsql_strerror(errorcode));
            break;
    }

    return errorcode;
}
