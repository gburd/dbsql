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

/* connection.h - definitions for the connection type
 *
 * Copyright (C) 2004-2006 Gerhard Häring <gh@ghaering.de>
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

#ifndef PYDBSQL_CONNECTION_H
#define PYDBSQL_CONNECTION_H
#include "Python.h"
#include "pythread.h"
#include "structmember.h"

#include "cache.h"
#include "module.h"

#include <dbsql.h>

typedef struct
{
    PyObject_HEAD
    DBSQL* db;

    /* 1 if we are currently within a transaction, i. e. if a BEGIN has been
     * issued */
    int inTransaction;

    /* the type detection mode. Only 0, PARSE_DECLTYPES, PARSE_COLNAMES or a
     * bitwise combination thereof makes sense */
    int detect_types;

    /* the timeout value in seconds for database locks */
    double timeout;

    /* for internal use in the timeout handler: when did the timeout handler
     * first get called with count=0? */
    double timeout_started;

    /* None for autocommit, otherwise a PyString with the isolation level */
    PyObject* isolation_level;

    /* NULL for autocommit, otherwise a string with the BEGIN statment; will be
     * freed in connection destructor */
    char* begin_statement;

    /* 1 if a check should be performed for each API call if the connection is
     * used from the same thread it was created in */
    int check_same_thread;

    /* thread identification of the thread the connection was created in */
    long thread_ident;

    pydbsql_Cache* statement_cache;

    /* A list of weak references to statements used within this connection */
    PyObject* statements;

    /* a counter for how many statements were created in the connection. May be
     * reset to 0 at certain intervals */
    int created_statements;

    PyObject* row_factory;

    /* Determines how bytestrings from DBSQL are converted to Python objects:
     * - PyUnicode_Type:        Python Unicode objects are constructed from UTF-8 bytestrings
     * - OptimizedUnicode:      Like before, but for ASCII data, only PyStrings are created.
     * - PyString_Type:         PyStrings are created as-is.
     * - Any custom callable:   Any object returned from the callable called with the bytestring
     *                          as single parameter.
     */
    PyObject* text_factory;

    /* remember references to functions/classes used in
     * create_function/create/aggregate, use these as dictionary keys, so we
     * can keep the total system refcount constant by clearing that dictionary
     * in connection_dealloc */
    PyObject* function_pinboard;

    /* a dictionary of registered collation name => collation callable mappings */
    PyObject* collations;

    /* Exception objects */
    PyObject* Warning;
    PyObject* Error;
    PyObject* InterfaceError;
    PyObject* DatabaseError;
    PyObject* DataError;
    PyObject* OperationalError;
    PyObject* IntegrityError;
    PyObject* InternalError;
    PyObject* ProgrammingError;
    PyObject* NotSupportedError;
} pydbsql_Connection;

extern PyTypeObject pydbsql_ConnectionType;

PyObject* pydbsql_connection_alloc(PyTypeObject* type, int aware);
void pydbsql_connection_dealloc(pydbsql_Connection* self);
PyObject* pydbsql_connection_cursor(pydbsql_Connection* self, PyObject* args, PyObject* kwargs);
PyObject* pydbsql_connection_close(pydbsql_Connection* self, PyObject* args);
PyObject* _pydbsql_connection_begin(pydbsql_Connection* self);
PyObject* pydbsql_connection_commit(pydbsql_Connection* self, PyObject* args);
PyObject* pydbsql_connection_rollback(pydbsql_Connection* self, PyObject* args);
PyObject* pydbsql_connection_new(PyTypeObject* type, PyObject* args, PyObject* kw);
int pydbsql_connection_init(pydbsql_Connection* self, PyObject* args, PyObject* kwargs);

int pydbsql_check_thread(pydbsql_Connection* self);
int pydbsql_check_connection(pydbsql_Connection* con);

int pydbsql_connection_setup_types(void);

#endif
