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

/* module.h - definitions for the module
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

#ifndef PYDBSQL_MODULE_H
#define PYDBSQL_MODULE_H
#include "Python.h"

#define PYDBSQL_VERSION "0.0.1"

extern PyObject* pydbsql_Error;
extern PyObject* pydbsql_Warning;
extern PyObject* pydbsql_InterfaceError;
extern PyObject* pydbsql_DatabaseError;
extern PyObject* pydbsql_InternalError;
extern PyObject* pydbsql_OperationalError;
extern PyObject* pydbsql_ProgrammingError;
extern PyObject* pydbsql_IntegrityError;
extern PyObject* pydbsql_DataError;
extern PyObject* pydbsql_NotSupportedError;

extern PyObject* pydbsql_OptimizedUnicode;

/* the functions time.time() and time.sleep() */
extern PyObject* time_time;
extern PyObject* time_sleep;

/* A dictionary, mapping colum types (INTEGER, VARCHAR, etc.) to converter
 * functions, that convert the SQL value to the appropriate Python value.
 * The key is uppercase.
 */
extern PyObject* converters;

extern int _enable_callback_tracebacks;

#define PARSE_DECLTYPES 1
#define PARSE_COLNAMES 2
#endif
