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

/* microprotocols.c - definitions for minimalist and non-validating protocols
 *
 * Copyright (C) 2003-2004 Federico Di Gregorio <fog@debian.org>
 *
 * This file is part of psycopg and was adapted for pysqlite. Federico Di
 * Gregorio gave the permission to use it within pysqlite under the following
 * license:
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

#ifndef PSYCOPG_MICROPROTOCOLS_H
#define PSYCOPG_MICROPROTOCOLS_H 1

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

/** adapters registry **/

extern PyObject *psyco_adapters;

/** the names of the three mandatory methods **/

#define MICROPROTOCOLS_GETQUOTED_NAME "getquoted"
#define MICROPROTOCOLS_GETSTRING_NAME "getstring"
#define MICROPROTOCOLS_GETBINARY_NAME "getbinary"

/** exported functions **/

/* used by module.c to init the microprotocols system */
extern int microprotocols_init(PyObject *dict);
extern int microprotocols_add(
    PyTypeObject *type, PyObject *proto, PyObject *cast);
extern PyObject *microprotocols_adapt(
    PyObject *obj, PyObject *proto, PyObject *alt);

extern PyObject *
    psyco_microprotocols_adapt(pydbsql_Cursor* self, PyObject *args);   
#define psyco_microprotocols_adapt_doc \
    "adapt(obj, protocol, alternate) -> adapt obj to given protocol. Non-standard."

#endif /* !defined(PSYCOPG_MICROPROTOCOLS_H) */
