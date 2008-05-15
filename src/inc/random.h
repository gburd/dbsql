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
 *
 * $Id: random.h 7 2007-02-03 13:34:17Z gburd $
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
