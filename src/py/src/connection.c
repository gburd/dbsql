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

/* connection.c - the connection type
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

#include "cache.h"
#include "module.h"
#include "connection.h"
#include "statement.h"
#include "cursor.h"
#include "prepare_protocol.h"
#include "util.h"
#include "dbsqlcompat.h"

#include "pythread.h"

static int pydbsql_connection_set_isolation_level(pydbsql_Connection* self, PyObject* isolation_level);


void _dbsql_result_error(dbsql_func_t* ctx, const char* errmsg, int len)
{
    dbsql_set_result_error(ctx, errmsg, len);
}

int pydbsql_connection_init(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    static char *kwlist[] = {"database", "timeout", "detect_types", "isolation_level", "check_same_thread", "factory", "cached_statements", NULL, NULL};

    char* database;
    char* errors;
    int detect_types = 0;
    PyObject* isolation_level = NULL;
    PyObject* factory = NULL;
    int check_same_thread = 1;
    int cached_statements = 100;
    double timeout = 5.0;
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|diOiOi", kwlist,
                                     &database, &timeout, &detect_types,
				     &isolation_level, &check_same_thread,
				     &factory, &cached_statements))
    {
        return -1; 
    }

    self->begin_statement = NULL;

    self->statement_cache = NULL;
    self->statements = NULL;

    Py_INCREF(Py_None);
    self->row_factory = Py_None;

    Py_INCREF(&PyUnicode_Type);
    self->text_factory = (PyObject*)&PyUnicode_Type;

    Py_BEGIN_ALLOW_THREADS
    rc = self->db->open(self->db, database, 0, &errors);
    Py_END_ALLOW_THREADS

    if (errors) {
	free(errors); /*TODO*/
	errors = 0;
    }
    if (rc != DBSQL_SUCCESS) {
        _pydbsql_seterror(rc);
        return -1;
    }

    if (!isolation_level) {
        isolation_level = PyString_FromString("");
        if (!isolation_level) {
            return -1;
        }
    } else {
        Py_INCREF(isolation_level);
    }
    self->isolation_level = NULL;
    pydbsql_connection_set_isolation_level(self, isolation_level);
    Py_DECREF(isolation_level);

    self->statement_cache = (pydbsql_Cache*)PyObject_CallFunction((PyObject*)&pydbsql_CacheType, "Oi", self, cached_statements);
    if (PyErr_Occurred()) {
        return -1;
    }

    self->statements = PyList_New(0);
    if (!self->statements) {
        return -1;
    }
    self->created_statements = 0;

    /* By default, the Cache class INCREFs the factory in its initializer, and
     * decrefs it in its deallocator method. Since this would create a circular
     * reference here, we're breaking it by decrementing self, and telling the
     * cache class to not decref the factory (self) in its deallocator.
     */
    self->statement_cache->decref_factory = 0;
    Py_DECREF(self);

    self->inTransaction = 0;
    self->detect_types = detect_types;
    self->timeout = timeout;
    self->db->set_timeout(self->db, (int)(timeout*1000));

    self->thread_ident = PyThread_get_thread_ident();
    self->check_same_thread = check_same_thread;

    self->function_pinboard = PyDict_New();
    if (!self->function_pinboard) {
        return -1;
    }

    self->collations = PyDict_New();
    if (!self->collations) {
        return -1;
    }

    self->Warning               = pydbsql_Warning;
    self->Error                 = pydbsql_Error;
    self->InterfaceError        = pydbsql_InterfaceError;
    self->DatabaseError         = pydbsql_DatabaseError;
    self->DataError             = pydbsql_DataError;
    self->OperationalError      = pydbsql_OperationalError;
    self->IntegrityError        = pydbsql_IntegrityError;
    self->InternalError         = pydbsql_InternalError;
    self->ProgrammingError      = pydbsql_ProgrammingError;
    self->NotSupportedError     = pydbsql_NotSupportedError;

    return 0;
}

/* Empty the entire statement cache of this connection */
void pydbsql_flush_statement_cache(pydbsql_Connection* self)
{
    pydbsql_Node* node;
    pydbsql_Statement* statement;

    node = self->statement_cache->first;

    while (node) {
        statement = (pydbsql_Statement*)(node->data);
        (void)pydbsql_statement_finalize(statement);
        node = node->next;
    }

    Py_DECREF(self->statement_cache);
    self->statement_cache = (pydbsql_Cache*)PyObject_CallFunction((PyObject*)&pydbsql_CacheType, "O", self);
    Py_DECREF(self);
    self->statement_cache->decref_factory = 0;
}

void pydbsql_reset_all_statements(pydbsql_Connection* self)
{
    int i;
    PyObject* weakref;
    PyObject* statement;

    for (i = 0; i < PyList_Size(self->statements); i++) {
        weakref = PyList_GetItem(self->statements, i);
        statement = PyWeakref_GetObject(weakref);
        if (statement != Py_None) {
            (void)pydbsql_statement_reset((pydbsql_Statement*)statement);
        }
    }
}

void pydbsql_connection_dealloc(pydbsql_Connection* self)
{
    Py_XDECREF(self->statement_cache);

    /* Clean up if user has not called .close() explicitly. */
    if (self->db) {
        Py_BEGIN_ALLOW_THREADS
	self->db->close(self->db);
        Py_END_ALLOW_THREADS
    }

    if (self->begin_statement) {
        PyMem_Free(self->begin_statement);
    }
    Py_XDECREF(self->isolation_level);
    Py_XDECREF(self->function_pinboard);
    Py_XDECREF(self->row_factory);
    Py_XDECREF(self->text_factory);
    Py_XDECREF(self->collations);
    Py_XDECREF(self->statements);

    self->ob_type->tp_free((PyObject*)self);
}

PyObject* pydbsql_connection_cursor(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    static char *kwlist[] = {"factory", NULL, NULL};
    PyObject* factory = NULL;
    PyObject* cursor;


    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwlist,
                                     &factory)) {
        return NULL;
    }

    if (!pydbsql_check_thread(self) || !pydbsql_check_connection(self)) {
        return NULL;
    }

    if (factory == NULL) {
        factory = (PyObject*)&pydbsql_CursorType;
    }

    cursor = PyObject_CallFunction(factory, "O", self);

    if (cursor && self->row_factory != Py_None) {
        Py_XDECREF(((pydbsql_Cursor*)cursor)->row_factory);
        Py_INCREF(self->row_factory);
        ((pydbsql_Cursor*)cursor)->row_factory = self->row_factory;
    }

    return cursor;
}

PyObject* pydbsql_connection_close(pydbsql_Connection* self, PyObject* args)
{
    int rc;

    if (!pydbsql_check_thread(self)) {
        return NULL;
    }

    pydbsql_flush_statement_cache(self);

    if (self->db) {
        Py_BEGIN_ALLOW_THREADS
	rc = self->db->close(self->db);
        Py_END_ALLOW_THREADS

        if (rc != DBSQL_SUCCESS) {
            _pydbsql_seterror(rc);
            return NULL;
        } else {
            self->db = NULL;
        }
    }

    Py_INCREF(Py_None);
    return Py_None;
}

/*
 * Checks if a connection object is usable (i. e. not closed).
 *
 * 0 => error; 1 => ok
 */
int pydbsql_check_connection(pydbsql_Connection* con)
{
    if (!con->db) {
        PyErr_SetString(pydbsql_ProgrammingError, "Cannot operate on a closed database.");
        return 0;
    } else {
        return 1;
    }
}

PyObject* _pydbsql_connection_begin(pydbsql_Connection* self)
{
    int rc;
    const char* tail;
    dbsql_stmt_t* statement;
    char *errors;

    Py_BEGIN_ALLOW_THREADS
    rc = self->db->prepare(self->db, self->begin_statement, &tail, &statement,
			   &errors);
    Py_END_ALLOW_THREADS

    if (errors) {
	free(errors); /*TODO*/
	errors = 0;
    }
    if (rc != DBSQL_SUCCESS) {
        _pydbsql_seterror(rc);
        goto error;
    }

    rc = _dbsql_step_with_busyhandler(statement, self);
    if (rc == DBSQL_DONE) {
        self->inTransaction = 1;
    } else {
        _pydbsql_seterror(rc);
    }

    Py_BEGIN_ALLOW_THREADS
    rc = self->db->finalize(statement, &errors);
    Py_END_ALLOW_THREADS

    if (errors) {
	free(errors); /*TODO*/
	errors = 0;
    }
    if (rc != DBSQL_SUCCESS && !PyErr_Occurred()) {
        _pydbsql_seterror(rc);
    }

error:
    if (PyErr_Occurred()) {
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

PyObject* pydbsql_connection_commit(pydbsql_Connection* self, PyObject* args)
{
    int rc;
    const char* tail;
    dbsql_stmt_t* statement;
    char *errors;

    if (!pydbsql_check_thread(self) || !pydbsql_check_connection(self)) {
        return NULL;
    }

    if (self->inTransaction) {
        Py_BEGIN_ALLOW_THREADS
        rc = self->db->prepare(self->db, "COMMIT", &tail, &statement, &errors);
        Py_END_ALLOW_THREADS

	if (errors) {
	    free(errors); /*TODO*/
	    errors = 0;
	}
        if (rc != DBSQL_SUCCESS) {
            _pydbsql_seterror(rc);
            goto error;
        }

        rc = _dbsql_step_with_busyhandler(statement, self);
        if (rc == DBSQL_DONE) {
            self->inTransaction = 0;
        } else {
            _pydbsql_seterror(rc);
        }

        Py_BEGIN_ALLOW_THREADS
        rc = self->db->finalize(statement, &errors);
        Py_END_ALLOW_THREADS
	if (errors) {
	    free(errors); /*TODO*/
	    errors = 0;
	}
        if (rc != DBSQL_SUCCESS && !PyErr_Occurred()) {
            _pydbsql_seterror(rc);
        }

    }

error:
    if (PyErr_Occurred()) {
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

PyObject* pydbsql_connection_rollback(pydbsql_Connection* self, PyObject* args)
{
    int rc;
    const char* tail;
    dbsql_stmt_t* statement;
    char *errors;

    if (!pydbsql_check_thread(self) || !pydbsql_check_connection(self)) {
        return NULL;
    }

    if (self->inTransaction) {
        pydbsql_reset_all_statements(self);

        Py_BEGIN_ALLOW_THREADS
        rc = self->db->prepare(self->db, "ROLLBACK", &tail, &statement,
			       &errors);
        Py_END_ALLOW_THREADS

	if (errors) {
	    free(errors); /*TODO*/
	    errors = 0;
	}
        if (rc != DBSQL_SUCCESS) {
            _pydbsql_seterror(rc);
            goto error;
        }

        rc = _dbsql_step_with_busyhandler(statement, self);
        if (rc == DBSQL_DONE) {
            self->inTransaction = 0;
        } else {
            _pydbsql_seterror(rc);
        }

        Py_BEGIN_ALLOW_THREADS
        rc = self->db->finalize(statement, &errors);
        Py_END_ALLOW_THREADS

	if (errors) {
	    free(errors); /*TODO*/
	    errors = 0;
	}
        if (rc != DBSQL_SUCCESS && !PyErr_Occurred()) {
            _pydbsql_seterror(rc);
        }

    }

error:
    if (PyErr_Occurred()) {
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

void _pydbsql_set_result(dbsql_func_t* context, PyObject* py_val)
{
    long longval;
    const char* buffer;
    Py_ssize_t buflen;
    PyObject* stringval;

    if ((!py_val) || PyErr_Occurred()) {
        dbsql_set_result_null(context);
    } else if (py_val == Py_None) {
        dbsql_set_result_null(context);
    } else if (PyInt_Check(py_val)) {
        longval = PyInt_AsLong(py_val);
        dbsql_set_result_int64(context, (PY_LONG_LONG)longval);
    } else if (PyFloat_Check(py_val)) {
        dbsql_set_result_double(context, PyFloat_AsDouble(py_val));
    } else if (PyBuffer_Check(py_val)) {
        if (PyObject_AsCharBuffer(py_val, &buffer, &buflen) != 0) {
            PyErr_SetString(PyExc_ValueError, "could not convert BLOB to buffer");
        } else {
            dbsql_set_result_blob(context, buffer, buflen, DBSQL_TRANSIENT);
        }
    } else if (PyString_Check(py_val)) {
        dbsql_set_result_varchar(context, PyString_AsString(py_val), -1, DBSQL_TRANSIENT);
    } else if (PyUnicode_Check(py_val)) {
        stringval = PyUnicode_AsUTF8String(py_val);
        if (stringval) {
            dbsql_set_result_varchar(context, PyString_AsString(stringval), -1, DBSQL_TRANSIENT);
            Py_DECREF(stringval);
        }
    } else {
        /* TODO: raise error */
    }
}

PyObject* _pydbsql_build_py_params(dbsql_func_t *context, int argc, dbsql_value_t** argv)
{
    PyObject* args;
    int i;
    dbsql_value_t* cur_value;
    PyObject* cur_py_value;
    const char* val_str;

    args = PyTuple_New(argc);
    if (!args) {
        return NULL;
    }

    for (i = 0; i < argc; i++) {
        cur_value = argv[i];
#if 0
    PY_LONG_LONG val_int;
    Py_ssize_t buflen;
    void* raw_buffer;
        switch (dbsql_value_type(argv[i])) { /* TODO: types */
            case DBSQL_INTEGER:
                val_int = dbsql_value_int64(cur_value);
                cur_py_value = PyInt_FromLong((long)val_int);
                break;
            case DBSQL_FLOAT:
                cur_py_value = PyFloat_FromDouble(dbsql_value_double(cur_value));
                break;
            case DBSQL_VARCHAR:
                val_str = (const char*)dbsql_value_varchar(cur_value);
                cur_py_value = PyUnicode_DecodeUTF8(val_str, strlen(val_str), NULL);
                /* TODO: have a way to show errors here */
                if (!cur_py_value) {
                    PyErr_Clear();
                    Py_INCREF(Py_None);
                    cur_py_value = Py_None;
                }
                break;
            case DBSQL_BLOB:
                buflen = dbsql_value_bytes(cur_value);
                cur_py_value = PyBuffer_New(buflen);
                if (!cur_py_value) {
                    break;
                }
                if (PyObject_AsWriteBuffer(cur_py_value, &raw_buffer, &buflen)) {
                    Py_DECREF(cur_py_value);
                    cur_py_value = NULL;
                    break;
                }
                memcpy(raw_buffer, dbsql_value_blob(cur_value), buflen);
                break;
            case DBSQL_NULL:
            default:
                Py_INCREF(Py_None);
                cur_py_value = Py_None;
        }
#else
	val_str = (const char*)(cur_value);
	cur_py_value = PyString_FromString(val_str);
	if (!cur_py_value) {
		PyErr_Clear();
		Py_INCREF(Py_None);
		cur_py_value = Py_None;
	}
	break;
#endif

        if (!cur_py_value) {
            Py_DECREF(args);
            return NULL;
        }

        PyTuple_SetItem(args, i, cur_py_value);

    }

    return args;
}

void _pydbsql_func_callback(dbsql_func_t* context, int argc, dbsql_value_t** argv)
{
    PyObject* args;
    PyObject* py_func;
    PyObject* py_retval = NULL;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    py_func = (PyObject*)dbsql_user_data(context);

    args = _pydbsql_build_py_params(context, argc, argv);
    if (args) {
        py_retval = PyObject_CallObject(py_func, args);
        Py_DECREF(args);
    }

    if (py_retval) {
        _pydbsql_set_result(context, py_retval);
        Py_DECREF(py_retval);
    } else {
        if (_enable_callback_tracebacks) {
            PyErr_Print();
        } else {
            PyErr_Clear();
        }
        _dbsql_result_error(context, "user-defined function raised exception", -1);
    }

    PyGILState_Release(threadstate);
}

static void _pydbsql_step_callback(dbsql_func_t *context, int argc, dbsql_value_t** params)
{
    PyObject* args;
    PyObject* function_result = NULL;
    PyObject* aggregate_class;
    PyObject** aggregate_instance;
    PyObject* stepmethod = NULL;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    aggregate_class = (PyObject*)dbsql_user_data(context);

    aggregate_instance = (PyObject**)dbsql_aggregate_context(context, sizeof(PyObject*));

    if (*aggregate_instance == 0) {
        *aggregate_instance = PyObject_CallFunction(aggregate_class, "");

        if (PyErr_Occurred()) {
            *aggregate_instance = 0;
            if (_enable_callback_tracebacks) {
                PyErr_Print();
            } else {
                PyErr_Clear();
            }
            _dbsql_result_error(context, "user-defined aggregate's '__init__' method raised error", -1);
            goto error;
        }
    }

    stepmethod = PyObject_GetAttrString(*aggregate_instance, "step");
    if (!stepmethod) {
        goto error;
    }

    args = _pydbsql_build_py_params(context, argc, params);
    if (!args) {
        goto error;
    }

    function_result = PyObject_CallObject(stepmethod, args);
    Py_DECREF(args);

    if (!function_result) {
        if (_enable_callback_tracebacks) {
            PyErr_Print();
        } else {
            PyErr_Clear();
        }
        _dbsql_result_error(context, "user-defined aggregate's 'step' method raised error", -1);
    }

error:
    Py_XDECREF(stepmethod);
    Py_XDECREF(function_result);

    PyGILState_Release(threadstate);
}

void _pydbsql_final_callback(dbsql_func_t* context)
{
    PyObject* function_result = NULL;
    PyObject** aggregate_instance;
    PyObject* aggregate_class;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    aggregate_class = (PyObject*)dbsql_user_data(context);

    aggregate_instance = (PyObject**)dbsql_aggregate_context(context, sizeof(PyObject*));
    if (!*aggregate_instance) {
        /* this branch is executed if there was an exception in the aggregate's
         * __init__ */

        goto error;
    }

    function_result = PyObject_CallMethod(*aggregate_instance, "finalize", "");
    if (!function_result) {
        if (_enable_callback_tracebacks) {
            PyErr_Print();
        } else {
            PyErr_Clear();
        }
        _dbsql_result_error(context, "user-defined aggregate's 'finalize' method raised error", -1);
    } else {
        _pydbsql_set_result(context, function_result);
    }

error:
    Py_XDECREF(*aggregate_instance);
    Py_XDECREF(function_result);

    PyGILState_Release(threadstate);
}

void _pydbsql_drop_unused_statement_references(pydbsql_Connection* self)
{
    PyObject* new_list;
    PyObject* weakref;
    int i;

    /* we only need to do this once in a while */
    if (self->created_statements++ < 200) {
        return;
    }

    self->created_statements = 0;

    new_list = PyList_New(0);
    if (!new_list) {
        return;
    }

    for (i = 0; i < PyList_Size(self->statements); i++) {
        weakref = PyList_GetItem(self->statements, i);
        if (PyWeakref_GetObject(weakref) != Py_None) {
            if (PyList_Append(new_list, weakref) != 0) {
                Py_DECREF(new_list);
                return;
            }
        }
    }

    Py_DECREF(self->statements);
    self->statements = new_list;
}

PyObject* pydbsql_connection_create_function(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    static char *kwlist[] = {"name", "narg", "func", NULL, NULL};

    PyObject* func;
    char* name;
    int narg;
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "siO", kwlist,
                                     &name, &narg, &func))
    {
        return NULL;
    }

    rc = self->db->create_function(self->db, name, narg, DBSQL_UTF8_ENCODED,
				   (void*)func, _pydbsql_func_callback,
				   NULL , NULL);

    if (rc != DBSQL_SUCCESS) {
        PyErr_SetString(pydbsql_OperationalError, "Error creating function");
        return NULL;
    } else {
        PyDict_SetItem(self->function_pinboard, func, Py_None);

        Py_INCREF(Py_None);
        return Py_None;
    }
}

PyObject* pydbsql_connection_create_aggregate(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* aggregate_class;

    int n_arg;
    char* name;
    static char *kwlist[] = { "name", "n_arg", "aggregate_class", NULL };
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "siO:create_aggregate",
                                      kwlist, &name, &n_arg, &aggregate_class)) {
        return NULL;
    }

    rc = self->db->create_function(self->db, name, n_arg, DBSQL_UTF8_ENCODED,
				   (void*)aggregate_class, NULL,
				   &_pydbsql_step_callback,
				   &_pydbsql_final_callback);
    if (rc != DBSQL_SUCCESS) {
        PyErr_SetString(pydbsql_OperationalError, "Error creating aggregate");
        return NULL;
    } else {
        PyDict_SetItem(self->function_pinboard, aggregate_class, Py_None);

        Py_INCREF(Py_None);
        return Py_None;
    }
}

static int _authorizer_callback(void* user_arg, int action, const char* arg1, const char* arg2 , const char* dbname, const char* access_attempt_source)
{
    PyObject *ret;
    int rc;
    PyGILState_STATE gilstate;

    gilstate = PyGILState_Ensure();
    ret = PyObject_CallFunction((PyObject*)user_arg, "issss", action, arg1, arg2, dbname, access_attempt_source);

    if (!ret) {
        if (_enable_callback_tracebacks) {
            PyErr_Print();
        } else {
            PyErr_Clear();
        }

        rc = DBSQL_DENY;
    } else {
        if (PyInt_Check(ret)) {
            rc = (int)PyInt_AsLong(ret);
        } else {
            rc = DBSQL_DENY;
        }
        Py_DECREF(ret);
    }

    PyGILState_Release(gilstate);
    return rc;
}

PyObject* pydbsql_connection_set_authorizer(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* authorizer_cb;

    static char *kwlist[] = { "authorizer_callback", NULL };
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:set_authorizer",
                                      kwlist, &authorizer_cb)) {
        return NULL;
    }

    rc = self->db->set_authorizer(self->db, _authorizer_callback, (void*)authorizer_cb);

    if (rc != DBSQL_SUCCESS) {
        PyErr_SetString(pydbsql_OperationalError, "Error setting authorizer callback");
        return NULL;
    } else {
        PyDict_SetItem(self->function_pinboard, authorizer_cb, Py_None);

        Py_INCREF(Py_None);
        return Py_None;
    }
}

int pydbsql_check_thread(pydbsql_Connection* self)
{
    if (self->check_same_thread) {
        if (PyThread_get_thread_ident() != self->thread_ident) {
            PyErr_Format(pydbsql_ProgrammingError,
                        "DBSQL objects created in a thread can only be used in that same thread."
                        "The object was created in thread id %ld and this is thread id %ld",
                        self->thread_ident, PyThread_get_thread_ident());
            return 0;
        }

    }

    return 1;
}

static PyObject* pydbsql_connection_get_isolation_level(pydbsql_Connection* self, void* unused)
{
    Py_INCREF(self->isolation_level);
    return self->isolation_level;
}

static PyObject* pydbsql_connection_get_total_changes(pydbsql_Connection* self, void* unused)
{
    if (!pydbsql_check_connection(self)) {
        return NULL;
    } else {
        return Py_BuildValue("i", self->db->total_change_count(self->db));
    }
}

static int pydbsql_connection_set_isolation_level(pydbsql_Connection* self, PyObject* isolation_level)
{
    PyObject* res;
    PyObject* begin_statement;

    Py_XDECREF(self->isolation_level);

    if (self->begin_statement) {
        PyMem_Free(self->begin_statement);
        self->begin_statement = NULL;
    }

    if (isolation_level == Py_None) {
        Py_INCREF(Py_None);
        self->isolation_level = Py_None;

        res = pydbsql_connection_commit(self, NULL);
        if (!res) {
            return -1;
        }
        Py_DECREF(res);

        self->inTransaction = 0;
    } else {
        Py_INCREF(isolation_level);
        self->isolation_level = isolation_level;

        begin_statement = PyString_FromString("BEGIN ");
        if (!begin_statement) {
            return -1;
        }
        PyString_Concat(&begin_statement, isolation_level);
        if (!begin_statement) {
            return -1;
        }

        self->begin_statement = PyMem_Malloc(PyString_Size(begin_statement) + 2);
        if (!self->begin_statement) {
            return -1;
        }

        strcpy(self->begin_statement, PyString_AsString(begin_statement));
        Py_DECREF(begin_statement);
    }

    return 0;
}

PyObject* pydbsql_connection_call(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* sql;
    pydbsql_Statement* statement;
    PyObject* weakref;
    int rc;

    if (!PyArg_ParseTuple(args, "O", &sql)) {
        return NULL;
    }

    _pydbsql_drop_unused_statement_references(self);

    statement = PyObject_New(pydbsql_Statement, &pydbsql_StatementType);
    if (!statement) {
        return NULL;
    }

    rc = pydbsql_statement_create(statement, self, sql);

    if (rc != DBSQL_SUCCESS) {
        if (rc == PYDBSQL_TOO_MUCH_SQL) {
            PyErr_SetString(pydbsql_Warning, "You can only execute one statement at a time.");
        } else if (rc == PYDBSQL_SQL_WRONG_TYPE) {
            PyErr_SetString(pydbsql_Warning, "SQL is of wrong type. Must be string or unicode.");
        } else {
            _pydbsql_seterror(rc);
        }

        Py_DECREF(statement);
        statement = 0;
    } else {
        weakref = PyWeakref_NewRef((PyObject*)statement, NULL);
        if (!weakref) {
            Py_DECREF(statement);
            statement = 0;
            goto error;
        }

        if (PyList_Append(self->statements, weakref) != 0) {
            Py_DECREF(weakref);
            statement = 0;
            goto error;
        }

        Py_DECREF(weakref);
    }

error:
    return (PyObject*)statement;
}

PyObject* pydbsql_connection_execute(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* cursor = 0;
    PyObject* result = 0;
    PyObject* method = 0;

    cursor = PyObject_CallMethod((PyObject*)self, "cursor", "");
    if (!cursor) {
        goto error;
    }

    method = PyObject_GetAttrString(cursor, "execute");
    if (!method) {
        Py_DECREF(cursor);
        cursor = 0;
        goto error;
    }

    result = PyObject_CallObject(method, args);
    if (!result) {
        Py_DECREF(cursor);
        cursor = 0;
    }

error:
    Py_XDECREF(result);
    Py_XDECREF(method);

    return cursor;
}

PyObject* pydbsql_connection_executemany(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* cursor = 0;
    PyObject* result = 0;
    PyObject* method = 0;

    cursor = PyObject_CallMethod((PyObject*)self, "cursor", "");
    if (!cursor) {
        goto error;
    }

    method = PyObject_GetAttrString(cursor, "executemany");
    if (!method) {
        Py_DECREF(cursor);
        cursor = 0;
        goto error;
    }

    result = PyObject_CallObject(method, args);
    if (!result) {
        Py_DECREF(cursor);
        cursor = 0;
    }

error:
    Py_XDECREF(result);
    Py_XDECREF(method);

    return cursor;
}

PyObject* pydbsql_connection_executescript(pydbsql_Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* cursor = 0;
    PyObject* result = 0;
    PyObject* method = 0;

    cursor = PyObject_CallMethod((PyObject*)self, "cursor", "");
    if (!cursor) {
        goto error;
    }

    method = PyObject_GetAttrString(cursor, "executescript");
    if (!method) {
        Py_DECREF(cursor);
        cursor = 0;
        goto error;
    }

    result = PyObject_CallObject(method, args);
    if (!result) {
        Py_DECREF(cursor);
        cursor = 0;
    }

error:
    Py_XDECREF(result);
    Py_XDECREF(method);

    return cursor;
}

#if 0
/* ------------------------- COLLATION CODE ------------------------ */

static int
pydbsql_collation_callback(
        void* context,
        int text1_length, const void* text1_data,
        int text2_length, const void* text2_data)
{
    PyObject* callback = (PyObject*)context;
    PyObject* string1 = 0;
    PyObject* string2 = 0;
    PyGILState_STATE gilstate;

    PyObject* retval = NULL;
    int result = 0;

    gilstate = PyGILState_Ensure();

    if (PyErr_Occurred()) {
        goto finally;
    }

    string1 = PyString_FromStringAndSize((const char*)text1_data, text1_length);
    string2 = PyString_FromStringAndSize((const char*)text2_data, text2_length);

    if (!string1 || !string2) {
        goto finally; /* failed to allocate strings */
    }

    retval = PyObject_CallFunctionObjArgs(callback, string1, string2, NULL);

    if (!retval) {
        /* execution failed */
        goto finally;
    }

    result = PyInt_AsLong(retval);
    if (PyErr_Occurred()) {
        result = 0;
    }

finally:
    Py_XDECREF(string1);
    Py_XDECREF(string2);
    Py_XDECREF(retval);

    PyGILState_Release(gilstate);

    return result;
}
#endif

static PyObject *
pydbsql_connection_interrupt(pydbsql_Connection* self, PyObject* args)
{
    PyObject* retval = NULL;

    if (!pydbsql_check_connection(self)) {
        goto finally;
    }

    self->db->interrupt(self->db);

    Py_INCREF(Py_None);
    retval = Py_None;

finally:
    return retval;
}

static PyObject *
pydbsql_connection_create_collation(pydbsql_Connection* self, PyObject* args)
{
    PyObject* callable;
    PyObject* uppercase_name = 0;
    PyObject* name;
    PyObject* retval;
    char* chk;

    if (!pydbsql_check_thread(self) || !pydbsql_check_connection(self)) {
        goto finally;
    }

    if (!PyArg_ParseTuple(args, "O!O:create_collation(name, callback)", &PyString_Type, &name, &callable)) {
        goto finally;
    }

    uppercase_name = PyObject_CallMethod(name, "upper", "");
    if (!uppercase_name) {
        goto finally;
    }

    chk = PyString_AsString(uppercase_name);
    while (*chk) {
        if ((*chk >= '0' && *chk <= '9')
         || (*chk >= 'A' && *chk <= 'Z')
         || (*chk == '_'))
        {
            chk++;
        } else {
            PyErr_SetString(pydbsql_ProgrammingError, "invalid character in collation name");
            goto finally;
        }
    }

    if (callable != Py_None && !PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "parameter must be callable");
        goto finally;
    }

    if (callable != Py_None) {
        PyDict_SetItem(self->collations, uppercase_name, callable);
    } else {
        PyDict_DelItem(self->collations, uppercase_name);
    }


/* TODO:    rc = self->db->create_collation(self->db,
                    PyString_AsString(uppercase_name),
                    DBSQL_UTF8,
                    (callable != Py_None) ? callable : NULL,
                    (callable != Py_None) ? pydbsql_collation_callback : NULL);
    if (rc != DBSQL_SUCCESS) {
        PyDict_DelItem(self->collations, uppercase_name);
        _pydbsql_seterror(self->db);
        goto finally;
    }
*/
finally:
    Py_XDECREF(uppercase_name);

    if (PyErr_Occurred()) {
        retval = NULL;
    } else {
        Py_INCREF(Py_None);
        retval = Py_None;
    }

    return retval;
}

static char connection_doc[] =
PyDoc_STR("DBSQL database connection object.");

static PyGetSetDef connection_getset[] = {
    {"isolation_level",  (getter)pydbsql_connection_get_isolation_level, (setter)pydbsql_connection_set_isolation_level},
    {"total_changes",  (getter)pydbsql_connection_get_total_changes, (setter)0},
    {NULL}
};

static PyMethodDef connection_methods[] = {
    {"cursor", (PyCFunction)pydbsql_connection_cursor, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Return a cursor for the connection.")},
    {"close", (PyCFunction)pydbsql_connection_close, METH_NOARGS,
        PyDoc_STR("Closes the connection.")},
    {"commit", (PyCFunction)pydbsql_connection_commit, METH_NOARGS,
        PyDoc_STR("Commit the current transaction.")},
    {"rollback", (PyCFunction)pydbsql_connection_rollback, METH_NOARGS,
        PyDoc_STR("Roll back the current transaction.")},
    {"create_function", (PyCFunction)pydbsql_connection_create_function, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Creates a new function. Non-standard.")},
    {"create_aggregate", (PyCFunction)pydbsql_connection_create_aggregate, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Creates a new aggregate. Non-standard.")},
    {"set_authorizer", (PyCFunction)pydbsql_connection_set_authorizer, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Sets authorizer callback. Non-standard.")},
    {"execute", (PyCFunction)pydbsql_connection_execute, METH_VARARGS,
        PyDoc_STR("Executes a SQL statement. Non-standard.")},
    {"executemany", (PyCFunction)pydbsql_connection_executemany, METH_VARARGS,
        PyDoc_STR("Repeatedly executes a SQL statement. Non-standard.")},
    {"executescript", (PyCFunction)pydbsql_connection_executescript, METH_VARARGS,
        PyDoc_STR("Executes a multiple SQL statements at once. Non-standard.")},
    {"create_collation", (PyCFunction)pydbsql_connection_create_collation, METH_VARARGS,
        PyDoc_STR("Creates a collation function. Non-standard.")},
    {"interrupt", (PyCFunction)pydbsql_connection_interrupt, METH_NOARGS,
        PyDoc_STR("Abort any pending database operation. Non-standard.")},
    {NULL, NULL}
};

static struct PyMemberDef connection_members[] =
{
    {"Warning", T_OBJECT, offsetof(pydbsql_Connection, Warning), RO},
    {"Error", T_OBJECT, offsetof(pydbsql_Connection, Error), RO},
    {"InterfaceError", T_OBJECT, offsetof(pydbsql_Connection, InterfaceError), RO},
    {"DatabaseError", T_OBJECT, offsetof(pydbsql_Connection, DatabaseError), RO},
    {"DataError", T_OBJECT, offsetof(pydbsql_Connection, DataError), RO},
    {"OperationalError", T_OBJECT, offsetof(pydbsql_Connection, OperationalError), RO},
    {"IntegrityError", T_OBJECT, offsetof(pydbsql_Connection, IntegrityError), RO},
    {"InternalError", T_OBJECT, offsetof(pydbsql_Connection, InternalError), RO},
    {"ProgrammingError", T_OBJECT, offsetof(pydbsql_Connection, ProgrammingError), RO},
    {"NotSupportedError", T_OBJECT, offsetof(pydbsql_Connection, NotSupportedError), RO},
    {"row_factory", T_OBJECT, offsetof(pydbsql_Connection, row_factory)},
    {"text_factory", T_OBJECT, offsetof(pydbsql_Connection, text_factory)},
    {NULL}
};

PyTypeObject pydbsql_ConnectionType = {
        PyObject_HEAD_INIT(NULL)
        0,                                              /* ob_size */
        MODULE_NAME ".Connection",                      /* tp_name */
        sizeof(pydbsql_Connection),                    /* tp_basicsize */
        0,                                              /* tp_itemsize */
        (destructor)pydbsql_connection_dealloc,        /* tp_dealloc */
        0,                                              /* tp_print */
        0,                                              /* tp_getattr */
        0,                                              /* tp_setattr */
        0,                                              /* tp_compare */
        0,                                              /* tp_repr */
        0,                                              /* tp_as_number */
        0,                                              /* tp_as_sequence */
        0,                                              /* tp_as_mapping */
        0,                                              /* tp_hash */
        (ternaryfunc)pydbsql_connection_call,          /* tp_call */
        0,                                              /* tp_str */
        0,                                              /* tp_getattro */
        0,                                              /* tp_setattro */
        0,                                              /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,         /* tp_flags */
        connection_doc,                                 /* tp_doc */
        0,                                              /* tp_traverse */
        0,                                              /* tp_clear */
        0,                                              /* tp_richcompare */
        0,                                              /* tp_weaklistoffset */
        0,                                              /* tp_iter */
        0,                                              /* tp_iternext */
        connection_methods,                             /* tp_methods */
        connection_members,                             /* tp_members */
        connection_getset,                              /* tp_getset */
        0,                                              /* tp_base */
        0,                                              /* tp_dict */
        0,                                              /* tp_descr_get */
        0,                                              /* tp_descr_set */
        0,                                              /* tp_dictoffset */
        (initproc)pydbsql_connection_init,             /* tp_init */
        0,                                              /* tp_alloc */
        0,                                              /* tp_new */
        0                                               /* tp_free */
};

extern int pydbsql_connection_setup_types(void)
{
    pydbsql_ConnectionType.tp_new = PyType_GenericNew;
    return PyType_Ready(&pydbsql_ConnectionType);
}
