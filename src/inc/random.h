/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2004
 *	Sleepycat Software.  All rights reserved.
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
