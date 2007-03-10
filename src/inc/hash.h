/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: hash.h 7 2007-02-03 13:34:17Z gburd $
 */
/*
 * Copyright (c) 1990-2004
 *      Sleepycat Software.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Redistributions in any form must be accompanied by information on
 *    how to obtain complete source code for the DB software and any
 *    accompanying software that uses the DB software.  The source code
 *    must either be included in the distribution or be available for no
 *    more than the cost of distribution plus a nominal fee, and must be
 *    freely redistributable under reasonable conditions.  For an
 *    executable file, complete source code means the source code for all
 *    modules it contains.  It does not include source code for modules or
 *    files that typically accompany the major components of the operating
 *    system on which the executable file runs.
 *
 * THIS SOFTWARE IS PROVIDED BY ORACLE CORPORATION ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * NON-INFRINGEMENT, ARE DISCLAIMED.  IN NO EVENT SHALL ORACLE CORPORATION
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) 1990, 1993, 1994, 1995
 *The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Copyright (c) 1995, 1996
 *The President and Fellows of Harvard University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions 
 */

#ifndef	_DBSQL_HASH_H_
#define	_DBSQL_HASH_H_

/*
 * This is the header file for the generic hash-table implemenation.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/* Forward declarations of structures. */
typedef struct __hash hash_t;
typedef struct __hash_ele hash_ele_t;

/*
 * A complete hash table is an instance of the following structure.
 * The internals of this structure are intended to be opaque -- client
 * code should not attempt to access or modify the fields of this structure
 * directly.  Change this structure only by using the routines below.
 * However, many of the "procedures" and "functions" for modifying and
 * accessing this structure are really macros, so we can't really make
 * this structure opaque.
 */
struct __hash {
	char keyClass;    /* DBSQL_HASH_INT, _POINTER, _STRING, _BINARY */
	char copyKey;     /* True if copy of key made on insert */
	int count;        /* Number of entries in this table */
	hash_ele_t *first;/* The first element of the array */
	int htsize;       /* Number of buckets in the hash table */
	struct _ht {      /* the hash table */
		int count;         /* Number of entries with this hash */
		hash_ele_t *chain; /* Pointer to first entry with this hash */
	} *ht;
};

/*
 * Each element in the hash table is an instance of the following 
 * structure.  All elements are stored on a single doubly-linked list.
 *
 * Again, this structure is intended to be opaque, but it can't really
 * be opaque because it is used by macros.
 */
struct __hash_ele {
	hash_ele_t *next, *prev;  /* Next and previous elements in the table */
	void *data;               /* Data associated with this element */
	void *pKey; int nKey;     /* Key associated with this element */
};

/*
 * There are 4 different modes of operation for a hash table:
 *
 *   DBSQL_HASH_INT         nKey is used as the key and pKey is ignored.
 *
 *   DBSQL_HASH_STRING      pKey points to a string that is nKey bytes long
 *                           (including the null-terminator, if any).  Case
 *                           is ignored in comparisons.
 *
 *   DBSQL_HASH_BINARY      pKey points to binary data nKey bytes long. 
 *                          __os_memcmp() is used to compare keys.
 *
 * A copy of the key is made for DBSQL_HASH_STRING and DBSQL_HASH_BINARY
 * if the copyKey parameter to HashInit is 1.  
 */
#define DBSQL_HASH_INT       1
#define DBSQL_HASH_STRING    3
#define DBSQL_HASH_BINARY    4

/*
 * Macros for looping over all elements of a hash table.  The idiom is
 * like this:
 *
 *   hash_t h;
 *   hash_ele_t *p;
 *   ...
 *   for(p=__hash_first(&h); p; p=__hash_next(p)){
 *     SomeStructure *pData = __hash_data(p);
 *     // do something with pData
 *   }
 */
#define __hash_first(H)  ((H)->first)
#define __hash_next(E)   ((E)->next)
#define __hash_data(E)   ((E)->data)
#define __hash_key(E)    ((E)->pKey)
#define __hash_keysize(E) ((E)->nKey)

/*
 * Number of entries in a hash table
 */
#define __hash_count(H)  ((H)->count)

#if defined(__cplusplus)
}
#endif
#endif /* !_DBSQL_HASH_H_ */
