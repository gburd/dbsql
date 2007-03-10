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

/* cache .c - a LRU cache
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
#include <limits.h>

/* only used internally */
pydbsql_Node* pydbsql_new_node(PyObject* key, PyObject* data)
{
    pydbsql_Node* node;

    node = (pydbsql_Node*) (pydbsql_NodeType.tp_alloc(&pydbsql_NodeType, 0));
    if (!node) {
        return NULL;
    }

    Py_INCREF(key);
    node->key = key;

    Py_INCREF(data);
    node->data = data;

    node->prev = NULL;
    node->next = NULL;

    return node;
}

void pydbsql_node_dealloc(pydbsql_Node* self)
{
    Py_DECREF(self->key);
    Py_DECREF(self->data);

    self->ob_type->tp_free((PyObject*)self);
}

int pydbsql_cache_init(pydbsql_Cache* self, PyObject* args, PyObject* kwargs)
{
    PyObject* factory;
    int size = 10;

    self->factory = NULL;

    if (!PyArg_ParseTuple(args, "O|i", &factory, &size)) {
        return -1;
    }

    /* minimum cache size is 5 entries */
    if (size < 5) {
        size = 5;
    }
    self->size = size;
    self->first = NULL;
    self->last = NULL;

    self->mapping = PyDict_New();
    if (!self->mapping) {
        return -1;
    }

    Py_INCREF(factory);
    self->factory = factory;

    self->decref_factory = 1;

    return 0;
}

void pydbsql_cache_dealloc(pydbsql_Cache* self)
{
    pydbsql_Node* node;
    pydbsql_Node* delete_node;

    if (!self->factory) {
        /* constructor failed, just get out of here */
        return;
    }

    /* iterate over all nodes and deallocate them */
    node = self->first;
    while (node) {
        delete_node = node;
        node = node->next;
        Py_DECREF(delete_node);
    }

    if (self->decref_factory) {
        Py_DECREF(self->factory);
    }
    Py_DECREF(self->mapping);

    self->ob_type->tp_free((PyObject*)self);
}

PyObject* pydbsql_cache_get(pydbsql_Cache* self, PyObject* args)
{
    PyObject* key = args;
    pydbsql_Node* node;
    pydbsql_Node* ptr;
    PyObject* data;

    node = (pydbsql_Node*)PyDict_GetItem(self->mapping, key);
    if (node) {
        /* an entry for this key already exists in the cache */

        /* increase usage counter of the node found */
        if (node->count < LONG_MAX) {
            node->count++;
        }

        /* if necessary, reorder entries in the cache by swapping positions */
        if (node->prev && node->count > node->prev->count) {
            ptr = node->prev;

            while (ptr->prev && node->count > ptr->prev->count) {
                ptr = ptr->prev;
            }

            if (node->next) {
                node->next->prev = node->prev;
            } else {
                self->last = node->prev;
            }
            if (node->prev) {
                node->prev->next = node->next;
            }
            if (ptr->prev) {
                ptr->prev->next = node;
            } else {
                self->first = node;
            }

            node->next = ptr;
            node->prev = ptr->prev;
            if (!node->prev) {
                self->first = node;
            }
            ptr->prev = node;
        }
    } else {
        /* There is no entry for this key in the cache, yet. We'll insert a new
         * entry in the cache, and make space if necessary by throwing the
         * least used item out of the cache. */

        if (PyDict_Size(self->mapping) == self->size) {
            if (self->last) {
                node = self->last;

                if (PyDict_DelItem(self->mapping, self->last->key) != 0) {
                    return NULL;
                }

                if (node->prev) {
                    node->prev->next = NULL;
                }
                self->last = node->prev;
                node->prev = NULL;

                Py_DECREF(node);
            }
        }

        data = PyObject_CallFunction(self->factory, "O", key);

        if (!data) {
            return NULL;
        }

        node = pydbsql_new_node(key, data);
        if (!node) {
            return NULL;
        }
        node->prev = self->last;

        Py_DECREF(data);

        if (PyDict_SetItem(self->mapping, key, (PyObject*)node) != 0) {
            Py_DECREF(node);
            return NULL;
        }

        if (self->last) {
            self->last->next = node;
        } else {
            self->first = node;
        }
        self->last = node;
    }

    Py_INCREF(node->data);
    return node->data;
}

PyObject* pydbsql_cache_display(pydbsql_Cache* self, PyObject* args)
{
    pydbsql_Node* ptr;
    PyObject* prevkey;
    PyObject* nextkey;
    PyObject* fmt_args;
    PyObject* template;
    PyObject* display_str;

    ptr = self->first;

    while (ptr) {
        if (ptr->prev) {
            prevkey = ptr->prev->key;
        } else {
            prevkey = Py_None;
        }
        Py_INCREF(prevkey);

        if (ptr->next) {
            nextkey = ptr->next->key;
        } else {
            nextkey = Py_None;
        }
        Py_INCREF(nextkey);

        fmt_args = Py_BuildValue("OOO", prevkey, ptr->key, nextkey);
        if (!fmt_args) {
            return NULL;
        }
        template = PyString_FromString("%s <- %s ->%s\n");
        if (!template) {
            return NULL;
        }
        display_str = PyString_Format(template, fmt_args);
        Py_DECREF(template);
        Py_DECREF(fmt_args);
        if (!display_str) {
            return NULL;
        }
        PyObject_Print(display_str, stdout, Py_PRINT_RAW);
        Py_DECREF(display_str);

        Py_DECREF(prevkey);
        Py_DECREF(nextkey);

        ptr = ptr->next;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef cache_methods[] = {
    {"get", (PyCFunction)pydbsql_cache_get, METH_O,
        PyDoc_STR("Gets an entry from the cache or calls the factory function to produce one.")},
    {"display", (PyCFunction)pydbsql_cache_display, METH_NOARGS,
        PyDoc_STR("For debugging only.")},
    {NULL, NULL}
};

PyTypeObject pydbsql_NodeType = {
        PyObject_HEAD_INIT(NULL)
        0,                                              /* ob_size */
        MODULE_NAME "Node",                             /* tp_name */
        sizeof(pydbsql_Node),                          /* tp_basicsize */
        0,                                              /* tp_itemsize */
        (destructor)pydbsql_node_dealloc,              /* tp_dealloc */
        0,                                              /* tp_print */
        0,                                              /* tp_getattr */
        0,                                              /* tp_setattr */
        0,                                              /* tp_compare */
        0,                                              /* tp_repr */
        0,                                              /* tp_as_number */
        0,                                              /* tp_as_sequence */
        0,                                              /* tp_as_mapping */
        0,                                              /* tp_hash */
        0,                                              /* tp_call */
        0,                                              /* tp_str */
        0,                                              /* tp_getattro */
        0,                                              /* tp_setattro */
        0,                                              /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,         /* tp_flags */
        0,                                              /* tp_doc */
        0,                                              /* tp_traverse */
        0,                                              /* tp_clear */
        0,                                              /* tp_richcompare */
        0,                                              /* tp_weaklistoffset */
        0,                                              /* tp_iter */
        0,                                              /* tp_iternext */
        0,                                              /* tp_methods */
        0,                                              /* tp_members */
        0,                                              /* tp_getset */
        0,                                              /* tp_base */
        0,                                              /* tp_dict */
        0,                                              /* tp_descr_get */
        0,                                              /* tp_descr_set */
        0,                                              /* tp_dictoffset */
        (initproc)0,                                    /* tp_init */
        0,                                              /* tp_alloc */
        0,                                              /* tp_new */
        0                                               /* tp_free */
};

PyTypeObject pydbsql_CacheType = {
        PyObject_HEAD_INIT(NULL)
        0,                                              /* ob_size */
        MODULE_NAME ".Cache",                           /* tp_name */
        sizeof(pydbsql_Cache),                         /* tp_basicsize */
        0,                                              /* tp_itemsize */
        (destructor)pydbsql_cache_dealloc,             /* tp_dealloc */
        0,                                              /* tp_print */
        0,                                              /* tp_getattr */
        0,                                              /* tp_setattr */
        0,                                              /* tp_compare */
        0,                                              /* tp_repr */
        0,                                              /* tp_as_number */
        0,                                              /* tp_as_sequence */
        0,                                              /* tp_as_mapping */
        0,                                              /* tp_hash */
        0,                                              /* tp_call */
        0,                                              /* tp_str */
        0,                                              /* tp_getattro */
        0,                                              /* tp_setattro */
        0,                                              /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,         /* tp_flags */
        0,                                              /* tp_doc */
        0,                                              /* tp_traverse */
        0,                                              /* tp_clear */
        0,                                              /* tp_richcompare */
        0,                                              /* tp_weaklistoffset */
        0,                                              /* tp_iter */
        0,                                              /* tp_iternext */
        cache_methods,                                  /* tp_methods */
        0,                                              /* tp_members */
        0,                                              /* tp_getset */
        0,                                              /* tp_base */
        0,                                              /* tp_dict */
        0,                                              /* tp_descr_get */
        0,                                              /* tp_descr_set */
        0,                                              /* tp_dictoffset */
        (initproc)pydbsql_cache_init,                  /* tp_init */
        0,                                              /* tp_alloc */
        0,                                              /* tp_new */
        0                                               /* tp_free */
};

extern int pydbsql_cache_setup_types(void)
{
    int rc;

    pydbsql_NodeType.tp_new = PyType_GenericNew;
    pydbsql_CacheType.tp_new = PyType_GenericNew;

    rc = PyType_Ready(&pydbsql_NodeType);
    if (rc < 0) {
        return rc;
    }

    rc = PyType_Ready(&pydbsql_CacheType);
    return rc;
}
