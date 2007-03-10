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

/* statement.h - definitions for the statement type
 *
 * Copyright (C) 2005 Gerhard Häring <gh@ghaering.de>
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

#ifndef PYDBSQL_STATEMENT_H
#define PYDBSQL_STATEMENT_H
#include "Python.h"

#include "connection.h"
#include <dbsql.h>

#define PYDBSQL_TOO_MUCH_SQL (-100)
#define PYDBSQL_SQL_WRONG_TYPE (-101)

typedef struct
{
    PyObject_HEAD
    DBSQL* db;
    dbsql_stmt_t* st;
    PyObject* sql;
    int in_use;
    PyObject* in_weakreflist; /* List of weak references */
} pydbsql_Statement;

extern PyTypeObject pydbsql_StatementType;

int pydbsql_statement_create(pydbsql_Statement* self, pydbsql_Connection* connection, PyObject* sql);
void pydbsql_statement_dealloc(pydbsql_Statement* self);

int pydbsql_statement_bind_parameter(pydbsql_Statement* self, int pos, PyObject* parameter);
void pydbsql_statement_bind_parameters(pydbsql_Statement* self, PyObject* parameters);

int pydbsql_statement_recompile(pydbsql_Statement* self, PyObject* parameters);
int pydbsql_statement_finalize(pydbsql_Statement* self);
int pydbsql_statement_reset(pydbsql_Statement* self);
void pydbsql_statement_mark_dirty(pydbsql_Statement* self);

int pydbsql_statement_setup_types(void);

#endif
