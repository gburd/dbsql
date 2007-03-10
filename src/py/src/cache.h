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

/* cache.h - definitions for the LRU cache
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

#ifndef PYDBSQL_CACHE_H
#define PYDBSQL_CACHE_H
#include "Python.h"

/* The LRU cache is implemented as a combination of a doubly-linked with a
 * dictionary. The list items are of type 'Node' and the dictionary has the
 * nodes as values. */

typedef struct _pydbsql_Node
{
    PyObject_HEAD
    PyObject* key;
    PyObject* data;
    long count;
    struct _pydbsql_Node* prev;
    struct _pydbsql_Node* next;
} pydbsql_Node;

typedef struct
{
    PyObject_HEAD
    int size;

    /* a dictionary mapping keys to Node entries */
    PyObject* mapping;

    /* the factory callable */
    PyObject* factory;

    pydbsql_Node* first;
    pydbsql_Node* last;

    /* if set, decrement the factory function when the Cache is deallocated.
     * this is almost always desirable, but not in the pydbsql context */
    int decref_factory;
} pydbsql_Cache;

extern PyTypeObject pydbsql_NodeType;
extern PyTypeObject pydbsql_CacheType;

int pydbsql_node_init(pydbsql_Node* self, PyObject* args, PyObject* kwargs);
void pydbsql_node_dealloc(pydbsql_Node* self);

int pydbsql_cache_init(pydbsql_Cache* self, PyObject* args, PyObject* kwargs);
void pydbsql_cache_dealloc(pydbsql_Cache* self);
PyObject* pydbsql_cache_get(pydbsql_Cache* self, PyObject* args);

int pydbsql_cache_setup_types(void);

#endif
