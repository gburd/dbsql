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
 *
 * $Id: xvprintf.h 7 2007-02-03 13:34:17Z gburd $
 */

#ifndef	_XVPRINTF_H_
#define	_XVPRINTF_H_

/*
 * A printf with some extra formatting features.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Undefine COMPATIBILITY to make some slight changes in the way things
 * work.  I think the changes are an improvement, but they are not
 * backwards compatible.
 */
/* #define COMPATIBILITY       / * Compatible with SUN OS 4.1 */

/*
 * This structure is used to store state information about the
 * write to memory that is currently in progress.
 */
typedef struct {
	char *base;     /* A base allocation */
	char *text;     /* The string collected so far */
	int  len;       /* Length of the string so far */
	int  amt;       /* Amount of space allocated in text */
} xvprintf_t;


#if defined(__cplusplus)
}
#endif
#endif /* !_ _H_ */
