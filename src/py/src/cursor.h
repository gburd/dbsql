/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  DBSQL Group, Inc.
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
 * $Id: api.c 7 2007-02-03 13:34:17Z gburd $
 */

/* cursor.h - definitions for the cursor type
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

#ifndef PYDBSQL_CURSOR_H
#define PYDBSQL_CURSOR_H
#include "Python.h"

#include "statement.h"
#include "connection.h"
#include "module.h"

typedef struct
{
    PyObject_HEAD
    pydbsql_Connection* connection;
    PyObject* description;
    PyObject* row_cast_map;
    int arraysize;
    PyObject* lastrowid;
    PyObject* rowcount;
    PyObject* row_factory;
    pydbsql_Statement* statement;

    /* the next row to be returned, NULL if no next row available */
    PyObject* next_row;
} pydbsql_Cursor;

typedef enum {
    STATEMENT_INVALID, STATEMENT_INSERT, STATEMENT_DELETE,
    STATEMENT_UPDATE, STATEMENT_REPLACE, STATEMENT_SELECT,
    STATEMENT_OTHER
} pydbsql_StatementKind;

extern PyTypeObject pydbsql_CursorType;

int pydbsql_cursor_init(pydbsql_Cursor* self, PyObject* args, PyObject* kwargs);
void pydbsql_cursor_dealloc(pydbsql_Cursor* self);
PyObject* pydbsql_cursor_execute(pydbsql_Cursor* self, PyObject* args);
PyObject* pydbsql_cursor_executemany(pydbsql_Cursor* self, PyObject* args);
PyObject* pydbsql_cursor_getiter(pydbsql_Cursor *self);
PyObject* pydbsql_cursor_iternext(pydbsql_Cursor *self);
PyObject* pydbsql_cursor_fetchone(pydbsql_Cursor* self, PyObject* args);
PyObject* pydbsql_cursor_fetchmany(pydbsql_Cursor* self, PyObject* args);
PyObject* pydbsql_cursor_fetchall(pydbsql_Cursor* self, PyObject* args);
PyObject* pydbsql_noop(pydbsql_Connection* self, PyObject* args);
PyObject* pydbsql_cursor_close(pydbsql_Cursor* self, PyObject* args);

int pydbsql_cursor_setup_types(void);

#define UNKNOWN (-1)
#endif
