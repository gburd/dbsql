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

/* microprotocols.c - minimalist and non-validating protocols implementation
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

#include <Python.h>
#include <structmember.h>

#include "cursor.h"
#include "microprotocols.h"
#include "prepare_protocol.h"


/** the adapters registry **/

PyObject *psyco_adapters;

/* microprotocols_init - initialize the adapters dictionary */

int
microprotocols_init(PyObject *dict)
{
    /* create adapters dictionary and put it in module namespace */
    if ((psyco_adapters = PyDict_New()) == NULL) {
        return -1;
    }

    return PyDict_SetItemString(dict, "adapters", psyco_adapters);
}


/* microprotocols_add - add a reverse type-caster to the dictionary */

int
microprotocols_add(PyTypeObject *type, PyObject *proto, PyObject *cast)
{
    PyObject* key;
    int rc;

    if (proto == NULL) proto = (PyObject*)&pydbsql_PrepareProtocolType;

    key = Py_BuildValue("(OO)", (PyObject*)type, proto);
    if (!key) {
        return -1;
    }

    rc = PyDict_SetItem(psyco_adapters, key, cast);
    Py_DECREF(key);

    return rc;
}

/* microprotocols_adapt - adapt an object to the built-in protocol */

PyObject *
microprotocols_adapt(PyObject *obj, PyObject *proto, PyObject *alt)
{
    PyObject *adapter, *key;

    /* we don't check for exact type conformance as specified in PEP 246
       because the pydbsql_PrepareProtocolType type is abstract and there is no
       way to get a quotable object to be its instance */

    /* look for an adapter in the registry */
    key = Py_BuildValue("(OO)", (PyObject*)obj->ob_type, proto);
    if (!key) {
        return NULL;
    }
    adapter = PyDict_GetItem(psyco_adapters, key);
    Py_DECREF(key);
    if (adapter) {
        PyObject *adapted = PyObject_CallFunctionObjArgs(adapter, obj, NULL);
        return adapted;
    }

    /* try to have the protocol adapt this object*/
    if (PyObject_HasAttrString(proto, "__adapt__")) {
        PyObject *adapted = PyObject_CallMethod(proto, "__adapt__", "O", obj);
        if (adapted) {
            if (adapted != Py_None) {
                return adapted;
            } else {
                Py_DECREF(adapted);
            }
        }

        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_TypeError))
            return NULL;
    }

    /* and finally try to have the object adapt itself */
    if (PyObject_HasAttrString(obj, "__conform__")) {
        PyObject *adapted = PyObject_CallMethod(obj, "__conform__","O", proto);
        if (adapted) {
            if (adapted != Py_None) {
                return adapted;
            } else {
                Py_DECREF(adapted);
            }
        }

        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_TypeError)) {
            return NULL;
        }
    }

    /* else set the right exception and return NULL */
    PyErr_SetString(pydbsql_ProgrammingError, "can't adapt");
    return NULL;
}

/** module-level functions **/

PyObject *
psyco_microprotocols_adapt(pydbsql_Cursor *self, PyObject *args)
{
    PyObject *obj, *alt = NULL;
    PyObject *proto = (PyObject*)&pydbsql_PrepareProtocolType;

    if (!PyArg_ParseTuple(args, "O|OO", &obj, &proto, &alt)) return NULL;
    return microprotocols_adapt(obj, proto, alt);
}
