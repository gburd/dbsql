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

/* module.c - the module itself
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

#include "connection.h"
#include "statement.h"
#include "cursor.h"
#include "cache.h"
#include "prepare_protocol.h"
#include "microprotocols.h"
#include "row.h"

#if DBSQL_VERSION_NUMBER >= 3003003
#define HAVE_SHARED_CACHE
#endif

/* static objects at module-level */

PyObject* pydbsql_Error, *pydbsql_Warning, *pydbsql_InterfaceError, *pydbsql_DatabaseError,
    *pydbsql_InternalError, *pydbsql_OperationalError, *pydbsql_ProgrammingError,
    *pydbsql_IntegrityError, *pydbsql_DataError, *pydbsql_NotSupportedError, *pydbsql_OptimizedUnicode;

PyObject* converters;
int _enable_callback_tracebacks;

static PyObject* module_connect(PyObject* self, PyObject* args, PyObject*
        kwargs)
{
    /* Python seems to have no way of extracting a single keyword-arg at
     * C-level, so this code is redundant with the one in connection_init in
     * connection.c and must always be copied from there ... */

    static char *kwlist[] = {"database", "timeout", "detect_types", "isolation_level", "check_same_thread", "factory", "cached_statements", NULL, NULL};
    char* database;
    int detect_types = 0;
    PyObject* isolation_level;
    PyObject* factory = NULL;
    int check_same_thread = 1;
    int cached_statements;
    double timeout = 5.0;

    PyObject* result;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|diOiOi", kwlist,
                                     &database, &timeout, &detect_types, &isolation_level, &check_same_thread, &factory, &cached_statements))
    {
        return NULL; 
    }

    if (factory == NULL) {
        factory = (PyObject*)&pydbsql_ConnectionType;
    }

    result = PyObject_Call(factory, args, kwargs);

    return result;
}

static PyObject* module_complete(PyObject* self, PyObject* args, PyObject*
        kwargs)
{
    static char *kwlist[] = {"statement", NULL, NULL};
    char* statement;

    PyObject* result;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s", kwlist, &statement))
    {
        return NULL; 
    }

    if (dbsql_complete_stmt(statement)) {
        result = Py_True;
    } else {
        result = Py_False;
    }

    Py_INCREF(result);

    return result;
}

#ifdef HAVE_SHARED_CACHE
static PyObject* module_enable_shared_cache(PyObject* self, PyObject* args, PyObject*
        kwargs)
{
    static char *kwlist[] = {"do_enable", NULL, NULL};
    int do_enable;
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i", kwlist, &do_enable))
    {
        return NULL; 
    }

    rc = dbsql_enable_shared_cache(do_enable);

    if (rc != DBSQL_SUCCESS) {
        PyErr_SetString(pydbsql_OperationalError, "Changing the shared_cache flag failed");
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}
#endif /* HAVE_SHARED_CACHE */

static PyObject* module_register_adapter(PyObject* self, PyObject* args, PyObject* kwargs)
{
    PyTypeObject* type;
    PyObject* caster;

    if (!PyArg_ParseTuple(args, "OO", &type, &caster)) {
        return NULL;
    }

    microprotocols_add(type, (PyObject*)&pydbsql_PrepareProtocolType, caster);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* module_register_converter(PyObject* self, PyObject* args, PyObject* kwargs)
{
    PyObject* orig_name;
    PyObject* name = NULL;
    PyObject* callable;
    PyObject* retval = NULL;

    if (!PyArg_ParseTuple(args, "SO", &orig_name, &callable)) {
        return NULL;
    }

    /* convert the name to upper case */
    name = PyObject_CallMethod(orig_name, "upper", "");
    if (!name) {
        goto error;
    }

    if (PyDict_SetItem(converters, name, callable) != 0) {
        goto error;
    }

    Py_INCREF(Py_None);
    retval = Py_None;
error:
    Py_XDECREF(name);
    return retval;
}

static PyObject* enable_callback_tracebacks(PyObject* self, PyObject* args, PyObject* kwargs)
{
    if (!PyArg_ParseTuple(args, "i", &_enable_callback_tracebacks)) {
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

static void converters_init(PyObject* dict)
{
    converters = PyDict_New();
    if (!converters) {
        return;
    }

    PyDict_SetItemString(dict, "converters", converters);
}

static PyMethodDef module_methods[] = {
    {"connect",  (PyCFunction)module_connect,  METH_VARARGS|METH_KEYWORDS, PyDoc_STR("Creates a connection.")},
    {"complete_statement",  (PyCFunction)module_complete,  METH_VARARGS|METH_KEYWORDS, PyDoc_STR("Checks if a string contains a complete SQL statement. Non-standard.")},
#ifdef HAVE_SHARED_CACHE
    {"enable_shared_cache",  (PyCFunction)module_enable_shared_cache,  METH_VARARGS|METH_KEYWORDS, PyDoc_STR("Enable or disable shared cache mode for the calling thread. Experimental/Non-standard.")},
#endif
    {"register_adapter", (PyCFunction)module_register_adapter, METH_VARARGS, PyDoc_STR("Registers an adapter with pydbsql's adapter registry. Non-standard.")},
    {"register_converter", (PyCFunction)module_register_converter, METH_VARARGS, PyDoc_STR("Registers a converter with pydbsql. Non-standard.")},
    {"adapt",  (PyCFunction)psyco_microprotocols_adapt, METH_VARARGS, psyco_microprotocols_adapt_doc},
    {"enable_callback_tracebacks",  (PyCFunction)enable_callback_tracebacks, METH_VARARGS, PyDoc_STR("Enable or disable callback functions throwing errors to stderr.")},
    {NULL, NULL}
};

struct _IntConstantPair {
    char* constant_name;
    int constant_value;
};

typedef struct _IntConstantPair IntConstantPair;

static IntConstantPair _int_constants[] = {
    {"PARSE_DECLTYPES", PARSE_DECLTYPES},
    {"PARSE_COLNAMES", PARSE_COLNAMES},

    {"DBSQL_SUCCESS", DBSQL_SUCCESS},
    {"DBSQL_DENY", DBSQL_DENY},
    {"DBSQL_IGNORE", DBSQL_IGNORE},
    {"DBSQL_CREATE_INDEX", DBSQL_CREATE_INDEX},
    {"DBSQL_CREATE_TABLE", DBSQL_CREATE_TABLE},
    {"DBSQL_CREATE_TEMP_INDEX", DBSQL_CREATE_TEMP_INDEX},
    {"DBSQL_CREATE_TEMP_TABLE", DBSQL_CREATE_TEMP_TABLE},
    {"DBSQL_CREATE_TEMP_TRIGGER", DBSQL_CREATE_TEMP_TRIGGER},
    {"DBSQL_CREATE_TEMP_VIEW", DBSQL_CREATE_TEMP_VIEW},
    {"DBSQL_CREATE_TRIGGER", DBSQL_CREATE_TRIGGER},
    {"DBSQL_CREATE_VIEW", DBSQL_CREATE_VIEW},
    {"DBSQL_DELETE", DBSQL_DELETE},
    {"DBSQL_DROP_INDEX", DBSQL_DROP_INDEX},
    {"DBSQL_DROP_TABLE", DBSQL_DROP_TABLE},
    {"DBSQL_DROP_TEMP_INDEX", DBSQL_DROP_TEMP_INDEX},
    {"DBSQL_DROP_TEMP_TABLE", DBSQL_DROP_TEMP_TABLE},
    {"DBSQL_DROP_TEMP_TRIGGER", DBSQL_DROP_TEMP_TRIGGER},
    {"DBSQL_DROP_TEMP_VIEW", DBSQL_DROP_TEMP_VIEW},
    {"DBSQL_DROP_TRIGGER", DBSQL_DROP_TRIGGER},
    {"DBSQL_DROP_VIEW", DBSQL_DROP_VIEW},
    {"DBSQL_INSERT", DBSQL_INSERT},
    {"DBSQL_PRAGMA", DBSQL_PRAGMA},
    {"DBSQL_READ", DBSQL_READ},
    {"DBSQL_SELECT", DBSQL_SELECT},
    {"DBSQL_TRANSACTION", DBSQL_TRANSACTION},
    {"DBSQL_UPDATE", DBSQL_UPDATE},
    {"DBSQL_ATTACH", DBSQL_ATTACH},
    {"DBSQL_DETACH", DBSQL_DETACH},
#if 0 /* TODO */
    {"DBSQL_ALTER_TABLE", DBSQL_ALTER_TABLE},
    {"DBSQL_REINDEX", DBSQL_REINDEX},
    {"DBSQL_ANALYZE", DBSQL_ANALYZE},
#endif
    {(char*)NULL, 0}
};

PyMODINIT_FUNC init_dbsql(void)
{
    PyObject *module, *dict;
    PyObject *tmp_obj;
    int i;

    module = Py_InitModule("pydbsql2._dbsql", module_methods);

    if (!module ||
        (pydbsql_row_setup_types() < 0) ||
        (pydbsql_cursor_setup_types() < 0) ||
        (pydbsql_connection_setup_types() < 0) ||
        (pydbsql_cache_setup_types() < 0) ||
        (pydbsql_statement_setup_types() < 0) ||
        (pydbsql_prepare_protocol_setup_types() < 0)
       ) {
        return;
    }

    Py_INCREF(&pydbsql_ConnectionType);
    PyModule_AddObject(module, "Connection", (PyObject*) &pydbsql_ConnectionType);
    Py_INCREF(&pydbsql_CursorType);
    PyModule_AddObject(module, "Cursor", (PyObject*) &pydbsql_CursorType);
    Py_INCREF(&pydbsql_CacheType);
    PyModule_AddObject(module, "Statement", (PyObject*)&pydbsql_StatementType);
    Py_INCREF(&pydbsql_StatementType);
    PyModule_AddObject(module, "Cache", (PyObject*) &pydbsql_CacheType);
    Py_INCREF(&pydbsql_PrepareProtocolType);
    PyModule_AddObject(module, "PrepareProtocol", (PyObject*) &pydbsql_PrepareProtocolType);
    Py_INCREF(&pydbsql_RowType);
    PyModule_AddObject(module, "Row", (PyObject*) &pydbsql_RowType);

    if (!(dict = PyModule_GetDict(module))) {
        goto error;
    }

    /*** Create DB-API Exception hierarchy */

    if (!(pydbsql_Error = PyErr_NewException(MODULE_NAME ".Error", PyExc_StandardError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "Error", pydbsql_Error);

    if (!(pydbsql_Warning = PyErr_NewException(MODULE_NAME ".Warning", PyExc_StandardError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "Warning", pydbsql_Warning);

    /* Error subclasses */

    if (!(pydbsql_InterfaceError = PyErr_NewException(MODULE_NAME ".InterfaceError", pydbsql_Error, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "InterfaceError", pydbsql_InterfaceError);

    if (!(pydbsql_DatabaseError = PyErr_NewException(MODULE_NAME ".DatabaseError", pydbsql_Error, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "DatabaseError", pydbsql_DatabaseError);

    /* pydbsql_DatabaseError subclasses */

    if (!(pydbsql_InternalError = PyErr_NewException(MODULE_NAME ".InternalError", pydbsql_DatabaseError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "InternalError", pydbsql_InternalError);

    if (!(pydbsql_OperationalError = PyErr_NewException(MODULE_NAME ".OperationalError", pydbsql_DatabaseError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "OperationalError", pydbsql_OperationalError);

    if (!(pydbsql_ProgrammingError = PyErr_NewException(MODULE_NAME ".ProgrammingError", pydbsql_DatabaseError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "ProgrammingError", pydbsql_ProgrammingError);

    if (!(pydbsql_IntegrityError = PyErr_NewException(MODULE_NAME ".IntegrityError", pydbsql_DatabaseError,NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "IntegrityError", pydbsql_IntegrityError);

    if (!(pydbsql_DataError = PyErr_NewException(MODULE_NAME ".DataError", pydbsql_DatabaseError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "DataError", pydbsql_DataError);

    if (!(pydbsql_NotSupportedError = PyErr_NewException(MODULE_NAME ".NotSupportedError", pydbsql_DatabaseError, NULL))) {
        goto error;
    }
    PyDict_SetItemString(dict, "NotSupportedError", pydbsql_NotSupportedError);

    /* We just need "something" unique for pydbsql_OptimizedUnicode. It does not really
     * need to be a string subclass. Just anything that can act as a special
     * marker for us. So I pulled PyCell_Type out of my magic hat.
     */
    Py_INCREF((PyObject*)&PyCell_Type);
    pydbsql_OptimizedUnicode = (PyObject*)&PyCell_Type;
    PyDict_SetItemString(dict, "OptimizedUnicode", pydbsql_OptimizedUnicode);

    /* Set integer constants */
    for (i = 0; _int_constants[i].constant_name != 0; i++) {
        tmp_obj = PyInt_FromLong(_int_constants[i].constant_value);
        if (!tmp_obj) {
            goto error;
        }
        PyDict_SetItemString(dict, _int_constants[i].constant_name, tmp_obj);
        Py_DECREF(tmp_obj);
    }

    if (!(tmp_obj = PyString_FromString(PYDBSQL_VERSION))) {
        goto error;
    }
    PyDict_SetItemString(dict, "version", tmp_obj);
    Py_DECREF(tmp_obj);

    if (!(tmp_obj = PyString_FromString(DBSQL_VERSION_STRING))) {
        goto error;
    }
    PyDict_SetItemString(dict, "dbsql_version", tmp_obj);
    Py_DECREF(tmp_obj);

    /* initialize microprotocols layer */
    microprotocols_init(dict);

    /* initialize the default converters */
    converters_init(dict);

    _enable_callback_tracebacks = 0;

    /* Original comment form _bsddb.c in the Python core. This is also still
     * needed nowadays for Python 2.3/2.4.
     * 
     * PyEval_InitThreads is called here due to a quirk in python 1.5
     * - 2.2.1 (at least) according to Russell Williamson <merel@wt.net>:
     * The global interepreter lock is not initialized until the first
     * thread is created using thread.start_new_thread() or fork() is
     * called.  that would cause the ALLOW_THREADS here to segfault due
     * to a null pointer reference if no threads or child processes
     * have been created.  This works around that and is a no-op if
     * threads have already been initialized.
     *  (see pybsddb-users mailing list post on 2002-08-07)
     */
    PyEval_InitThreads();

error:
    if (PyErr_Occurred())
    {
        PyErr_SetString(PyExc_ImportError, "pydbsql2._dbsql: init failed");
    }
}
