/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
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
