/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
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

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdio.h>
#endif

#include "dbsql_int.h"

#define SEED_SZ 256

/*
 * __rng_seed --
 *	Produce a sutable seed for our random number generator.  In
 *	this case that means using the current time.  The seed is
 *	written to a char[SEED_SZ] buffer passed by the caller.
 *	Note that when testing its useful to generate the same pattern
 *	of random numbers.  We accomplish that by initializing the contents
 *	of 'buf' to '\0' when CONFIG_TEST is defined.  In this way, tests
 *	are repeatable.
 */
#ifndef HAVE_SRAND48_R
void
__rng_seed(buf)
	char *buf;
{
	u_int32_t pid;
	double jt;
	u_int32_t i;

	memset(buf, 0, SEED_SZ);
#ifdef CONFIG_TEST
	return;
#else
	__os_id(NULL, &pid, NULL);
	__os_jtime(&jt);
	for (i = 0; i < SEED_SZ; i++) {
		if (i % 2)
			buf[i] = (char)(pid & 0xf);
		else
			buf[i] = (char)((u_int32_t)jt & 0xf);
	}
	return;
#endif
}
#endif

/*
 * srand48_r --
 *	Seed the random number generator before use.
 *
 * PUBLIC: #ifndef HAVE_SRAND48_R
 * PUBLIC: int srand48_r __P((struct drand48_data *));
 * PUBLIC: #endif
 */
#ifndef HAVE_SRAND48_R
int
srand48_r(buffer)
	struct drand48_data *buffer;
{
	int i;
	char k[SEED_SZ];
	buffer->j = 0;
	buffer->i = 0;
	__rng_seed(k);
	for(i = 0; i < 256; i++) {
		buffer->s[i] = i;
	}
	for(i = 0; i < 256; i++) {
		int t;
		buffer->j = (buffer->j + buffer->s[i] + k[i]) & 0xff;
		t = buffer->s[buffer->j];
		buffer->s[buffer->j] = buffer->s[i];
		buffer->s[i] = t;
	}
	buffer->init_p = 1;
}
#endif

/*
 * lrand48_r --
 *	A re-entrant version of a random number generator.  This
 *	random number generator is based on the RC4 algorithm.
 *
 * STATIC: #ifndef HAVE_LRAND48_R
 * STATIC: int lrand48_r __P((struct drand48_data *, double *));
 * STATIC: #endif
 */
#ifndef HAVE_LRAND48_R
static int
lrand48_r(buffer, result)
	struct drand48_data *buffer;
	double *result;
{
	/* TODO: am I sure there isn't a race in here? */
	int t;
	buffer->i = (buffer->i + 1) & 0xff;
	buffer->j = (buffer->j + buffer->s[buffer->i]) & 0xff;
	t = buffer->s[buffer->i];
	buffer->s[buffer->i] = buffer->s[buffer->j];
	buffer->s[buffer->j] = t;
	t = buffer->s[buffer->i] + buffer->s[buffer->j];
	*result = buffer->s[t & 0xff];
	return 0;
}
#endif

/*
 * rand8_r --
 *
 * PUBLIC: int rand8_r __P((struct drand48_data *, u_int8_t *));
 */
int
rand8_r(buffer, result)
	struct drand48_data *buffer;
	u_int8_t *result;
{
	long int i;
	int rc = lrand48_r(buffer, &i);
	*result = i & 0xf;
	return 0;
}

/*
 * rand32_r --
 *
 * PUBLIC: int rand32_r __P((struct drand48_data *, u_int32_t *));
 */
int
rand32_r(buffer, result)
	struct drand48_data *buffer;
	u_int32_t *result;
{
	int rc, i;
	long int r, s;
	rc = lrand48_r(buffer, &r);
	for(i = 1; i < 4; i++) {
		rc = lrand48_r(buffer, &s);
		r = (r << 8) + s;
	}
	*result = r;
	return rc;
}

