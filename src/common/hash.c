/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
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

/*
 * Basic hash-tables.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __hash_init --
 *	Turn bulk memory into a hash table object by initializing the
 *	fields of the hash_t structure.
 *
 *	'this' is a pointer to the hash table that is to be initialized.
 *	'class' is one of the constants DBSQL_HASH_INT, DBSQL_HASH_POINTER,
 *	DBSQL_HASH_BINARY, or DBSQL_HASH_STRING.  The value of 'class' 
 *	determines what kind of key the hash table will use.  'copy_key_p' is
 *	true if the hash table should make its own private copy of keys and
 *	false if it should just use the supplied pointer.  'copy_key_p' only
 *	makes sense for DBSQL_HASH_STRING and DBSQL_HASH_BINARY and is ignored
 *	for other key classes.
 *
 * PUBLIC: void __hash_init __P((hash_t *, int, int));
 */
void
__hash_init(this, class, copy_key_p)
	hash_t *this;
	int class;
	int copy_key_p;
{
	DBSQL_ASSERT(this != 0);
	DBSQL_ASSERT(class >= DBSQL_HASH_INT && class <= DBSQL_HASH_BINARY);
	this->keyClass = class;
	this->copyKey = copy_key_p &&
                (class==DBSQL_HASH_STRING || class==DBSQL_HASH_BINARY);
	this->first = 0;
	this->count = 0;
	this->htsize = 0;
	this->ht = 0;
}

/*
 * __hash_clear --
 *	Remove all entries from a hash table.  Reclaim all memory.
 *	Call this routine to delete a hash table or to reset a hash table
 *	to the empty state.
 *
 * PUBLIC: void __hash_clear __P((hash_t *));
 */
void
__hash_clear(this)
	hash_t *this;
{
	hash_ele_t *elem;
	DBSQL_ASSERT(this != 0);
	elem = this->first;
	this->first = 0;
	if( this->ht )
		__dbsql_free(NULL, this->ht);
	this->ht = 0;
	this->htsize = 0;
	while(elem) {
		hash_ele_t *next_elem = elem->next;
		if( this->copyKey && elem->pKey ){
			__dbsql_free(NULL, elem->pKey);
		}
		__dbsql_free(NULL, elem);
		elem = next_elem;
	}
	this->count = 0;
}

/*
 * __int_hash --
 *	Hash and comparison functions when the mode is DBSQL_HASH_INT
 *
 * STATIC: static int __int_hash __P((const void *, int));
 */
static int
__int_hash(key, len)
	const void *key;
	int len;
{
	return len ^ (len << 8) ^ (len >> 8);
}

/*
 * __int_cmp --
 *
 * STATIC: static int __h_int_cmp __P((const void *, int, const void *, int));
 */
static int
__h_int_cmp(k1, n1, k2, n2)
	const void *k1;
	int n1;
	const void *k2;
	int n2;
{
	return n2 - n1;
}

/*
 * __str_hash --
 *	Hash and comparison functions when the mode is DBSQL_HASH_STRING
 *
 * STATIC: static int __str_hash
 */
static int
__str_hash(key, len)
	const void *key;
	int len;
{
	return __hash_ignore_case((const char*)key, len); 
}

/*
 * __str_cmp --
 *
 * STATIC: static int __h_str_cmp __P((const void *, int, const void *, int));
 */
static int
__h_str_cmp(k1, n1, k2, n2)
	const void *k1;
	int n1;
	const void *k2;
	int n2;
{
	if (n1 != n2)
		return (n2 - n1);
	return strncasecmp((const char*)k1, (const char*)k2, n1);
}

/*
 * __bin_hash --
 *	Hash and comparison functions when the mode is DBSQL_HASH_BINARY
 *
 * STATIC: static int __bin_hash __((const void *, int));
 */
static int
__bin_hash(key, len)
	const void *key;
	int len;
{
	int h = 0;
	const char *z = (const char *)key;
	while(len-- > 0) {
		h = (h << 3) ^ h ^ * (z++);
	}
	return h & 0x7fffffff;
}

/*
 * __bin_cmp --
 *
 * STATIC: static int __h_bin_cmp __P((const void *, int, const void *, int));
 */
static int
__h_bin_cmp(k1, n1, k2, n2)
	const void *k1;
	int n1;
	const void *k2;
	int n2;
{
	if (n1 != n2)
		return (n2 - n1);
	return memcmp(k1, k2, n1);
}

/*
 * __hash_fn --
 *	Return a pointer to the appropriate hash function given the key class.
 *
 *	The C syntax in this function definition may be unfamilar to some 
 *	programmers, so we provide the following additional explanation:
 *
 *	The name of the function is '__hash_fn'.  The function takes a
 *	single parameter 'class'.  The return value of __hash_fn()
 *	is a pointer to another function.  Specifically, the return value
 *	of __hash_fn() is a pointer to a function that takes two parameters
 *	with types "const void*" and "int" and returns an "int".
 */
static int
(*__hash_fn(int class))(const void*, int)
{
	switch(class) {
	case DBSQL_HASH_INT:
		return &__int_hash;
	case DBSQL_HASH_STRING:
		return &__str_hash;
	case DBSQL_HASH_BINARY:
		return &__bin_hash;;
	default:
		break;
	}
	return 0;
}

/*
 * __cmp_fn --
 *	Return a pointer to the appropriate hash function given the key class.
 *
 *	For help in interpreted the obscure C code in the function definition,
 *	see the header comment on the previous function.
 */
static int
(*__cmp_fn(int class))(const void*, int, const void*, int)
{
	switch(class) {
	case DBSQL_HASH_INT:
		return &__h_int_cmp;
	case DBSQL_HASH_STRING:
		return &__h_str_cmp;
	case DBSQL_HASH_BINARY:
		return &__h_bin_cmp;
	default:
		break;
	}
	return 0;
}


/*
 * __rehash --
 *	Resize the hash table so that it cantains 'new_size' buckets.
 *	'new_size' must be a power of 2.  The hash table might fail 
 *	to resize if __dbsql_calloc() fails.
 *
 * STATIC: static void __rehash __P((hash_t *, int));
 */
static void
__rehash(this, new_size)
	hash_t *this;
	int new_size;
{
	struct _ht *new_ht;           /* The new hash table */
	hash_ele_t *elem, *next_elem; /* For looping over existing elements */
	hash_ele_t *x;                /* Element being copied to new hash
					 table */
	int (*hash)(const void*,int); /* The hash function */

	DBSQL_ASSERT((new_size & (new_size - 1)) == 0);
	if (__dbsql_calloc(NULL, new_size, sizeof(struct _ht), &new_ht) == ENOMEM)
		return;
	if (this->ht)
		__dbsql_free(NULL, this->ht);
	this->ht = new_ht;
	this->htsize = new_size;
	hash = __hash_fn(this->keyClass);
	for (elem = this->first, this->first = 0; elem; elem = next_elem) {
		int h = (*hash)(elem->pKey, elem->nKey) & (new_size - 1);
		next_elem = elem->next;
		x = new_ht[h].chain;
		if (x) {
			elem->next = x;
			elem->prev = x->prev;
			if (x->prev)
				x->prev->next = elem;
			else
				this->first = elem;
			x->prev = elem;
		} else {
			elem->next = this->first;
			if (this->first)
				this->first->prev = elem;
			elem->prev = 0;
			this->first = elem;
		}
		new_ht[h].chain = elem;
		new_ht[h].count++;
	}
}

/*
 * __hash_search --
 *	This function locates an element in an hash table that matches
 *	the given key.  The hash for this key has already been computed
 *	and is passed as the 4th parameter.
 *
 * STATIC: static hash_ele_t * __hash_search __P((const hast_t *, const void *,
 * STATIC:                     int, int));
 *
 * this				The hash_t to be searched
 * key				The the object of our search
 * len				The size of the key
 * h				The hash value for this key
 */
static hash_ele_t *__hash_search(this, key, len, h)
	const hash_t *this;
	const void *key;
	int len;
	int h;
{
	hash_ele_t *elem;
	int count;
	int (*cmp)(const void*, int, const void*, int);

	if (this->ht) {
		elem = this->ht[h].chain;
		count = this->ht[h].count;
		cmp = __cmp_fn(this->keyClass);
		while (count-- && elem) {
			if ((*cmp)(elem->pKey, elem->nKey, key, len) == 0) { 
				return elem;
			}
			elem = elem->next;
		}
	}
	return 0;
}

/*
 * __hash_remove --
 *	Remove a single entry from the hash table given a pointer to that
 *	element and a hash on the element's key.
 *
 * STATIC: static void __hash_remove __P((hash_t *, hash_ele_t *, int));
 *
 * this				The hash_t to be searched
 * elem				The element to be removed
 * h				The hash value for this key
 */
static void __hash_remove(this, elem, h)
	hash_t *this;
	hash_ele_t* elem;
	int h;
{
	if (elem->prev) {
		elem->prev->next = elem->next; 
	} else {
		this->first = elem->next;
	}
	if (elem->next) {
		elem->next->prev = elem->prev;
	}
	if (this->ht[h].chain == elem) {
		this->ht[h].chain = elem->next;
	}
	this->ht[h].count--;
	if (this->ht[h].count <= 0) {
		this->ht[h].chain = 0;
	}
	if (this->copyKey && elem->pKey) {
		__dbsql_free(NULL, elem->pKey);
	}
	__dbsql_free(NULL, elem);
	this->count--;
}

/*
 * __hash_find --
 *	Attempt to locate an element of the hash table this with a key
 *	that matches 'key', 'len'.  Return the data for this element if it is
 *	found, or NULL if there is no match.
 *
 * PUBLIC: void *__hash_find __P((const hash_t *, const void *, int));
 */
void *__hash_find(this, key, len)
	const hash_t *this;
	const void *key;
	int len;
{
	int h;
	hash_ele_t *elem;
	int (*hash)(const void*, int);

	if (this == 0 || this->ht == 0)
		return 0;
	hash = __hash_fn(this->keyClass);
	DBSQL_ASSERT(hash != 0);
	h = (*hash)(key, len);
	DBSQL_ASSERT((this->htsize & (this->htsize - 1)) == 0);
	elem = __hash_search(this, key, len, h & (this->htsize - 1));
	return (elem ? elem->data : 0);
}

/*
 * __hash_insert --
 *	Insert an element into the hash table this.  The key is 'key', 'len'
 *	and the data is 'data'.
 *
 *	If no element exists with a matching key, then a new
 *	element is created.  A copy of the key is made if the copy_key_p
 *	flag is set.  NULL is returned.
 *
 *	If another element already exists with the same key, then the
 *	new data replaces the old data and the old data is returned.
 *	The key is not copied in this instance.  If a __dbsql_malloc fails,
 *	then the new data is returned and the hash table is unchanged.
 *
 *	If the 'data' parameter to this function is NULL, then the
 *	 element corresponding to 'key' is removed from the hash table.
 *
 * PUBLIC: void *__hash_insert __P((hash_t *, const void *, int, void *));
 */
void *
__hash_insert(this, key, len, data)
	hash_t *this;
	const void *key;
	int len;
	void *data;
{
	int hraw;             /* Raw hash value of the key */
	int h;                /* the hash of the key modulo hash table size */
	hash_ele_t *elem;     /* Used to loop thru the element list */
	hash_ele_t *new_elem; /* New element added to the pH */
	int (*hash)(const void*,int); /* The hash function */

	DBSQL_ASSERT(this != 0);
	hash = __hash_fn(this->keyClass);
	DBSQL_ASSERT(hash != 0);
	hraw = (*hash)(key, len);
	DBSQL_ASSERT((this->htsize & (this->htsize - 1)) == 0);
	h = hraw & (this->htsize - 1);
	elem = __hash_search(this, key, len, h);
	if (elem) {
		void *old_data = elem->data;
		if (data == 0) {
			__hash_remove(this, elem, h);
		} else {
			elem->data = data;
		}
		return old_data;
	}
	if (data == 0)
		return 0;
	if (__dbsql_calloc(NULL, 1, sizeof(hash_ele_t), &new_elem) == ENOMEM)
		return data;
	if (this->copyKey && key != 0) {
		if (__dbsql_calloc(NULL, 1, len, &new_elem->pKey) == ENOMEM) {
			__dbsql_free(NULL, new_elem);
			return data;
		}
		memcpy((void*)new_elem->pKey, key, len);
	} else {
		new_elem->pKey = (void*)key;
	}
	new_elem->nKey = len;
	this->count++;
	if (this->htsize == 0)
		__rehash(this, 8);
	if (this->htsize == 0) {
		this->count = 0;
		__dbsql_free(NULL, new_elem);
		return data;
	}
	if (this->count > this->htsize) {
		__rehash(this, this->htsize * 2);
	}
	DBSQL_ASSERT((this->htsize & (this->htsize - 1)) == 0);
	h = hraw & (this->htsize - 1);
	elem = this->ht[h].chain;
	if (elem) {
		new_elem->next = elem;
		new_elem->prev = elem->prev;
		if (elem->prev) {
			elem->prev->next = new_elem;
		} else {
			this->first = new_elem;
		}
		elem->prev = new_elem;
	} else {
		new_elem->next = this->first;
		new_elem->prev = 0;
		if (this->first) {
			this->first->prev = new_elem;
		}
		this->first = new_elem;
	}
	this->ht[h].count++;
	this->ht[h].chain = new_elem;
	new_elem->data = data;
	return 0;
}

/*
 * __hash_ignore_case --
 *	This function computes a hash on the name of a keyword.
 *	Case is not significant.
 *
 * PUBLIC: int __hash_ignore_case __P((const char *, int));
 */
int
__hash_ignore_case(z, n)
	const char *z;
	int n;
{
	int h = 0;
	if (n <= 0)
		n = strlen(z);
	while(n > 0) {
		h = (h<<3) ^ h ^ __str_upper_to_lower[(unsigned char) * z++];
		n--;
	}
	return h & 0x7fffffff;
}
