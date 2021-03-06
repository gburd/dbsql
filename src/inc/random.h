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

#ifndef	_LRAND48_R_H_
#define	_LRAND48_R_H_

/*
 * A re-entrant version of a random number generator.  This
 * random number generator is based on the RC4 algorithm.
 */

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef HAVE_SRAND48_R
struct drand48_data {
	int init_p;          /* True if initialized */
	int i, j;            /* State variables */
	int s[256];          /* State variables */
};
#endif

#if defined(__cplusplus)
}
#endif
#endif /* !_SRAND48_R_H_ */
