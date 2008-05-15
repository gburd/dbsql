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
 * $Id: dbsql_atof.c 7 2007-02-03 13:34:17Z gburd $
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __dbsql_atof --
 *	The string z[] is an ascii representation of a real number.
 *	Convert this string to a double.
 *
 *	This routine assumes that z[] really is a valid number.  If it
 *	is not, the result is undefined.
 *
 *	This routine is used instead of the library atof() function because
 *	the library atof() might want to use "," as the decimal point instead
 *	of "." depending on how locale is set.  But that would cause problems
 *	for SQL.  So this routine always uses "." regardless of locale.
 *
 * PUBLIC: double __dbsql_atof __P((const char *));
 */
double
__dbsql_atof(z)
	const char *z;
{
	int sign = 1;
	long_double_t v1 = 0.0;
	if (*z == '-') {
		sign = -1;
		z++;
	} else if (*z == '+') {
		z++;
	}
	while(isdigit(*z)) {
		v1 = v1 * 10.0 + (*z - '0');
		z++;
	}
	if (*z == '.') {
		long_double_t divisor = 1.0;
		z++;
		while(isdigit(*z)) {
			v1 = v1 * 10.0 + (*z - '0');
			divisor *= 10.0;
			z++;
		}
		v1 /= divisor;
	}
	if (*z == 'e' || *z == 'E') {
		int esign = 1;
		int eval = 0;
		long_double_t scale = 1.0;
		z++;
		if (*z == '-') {
			esign = -1;
			z++;
		} else if (*z == '+') {
			z++;
		}
		while(isdigit(*z)) {
			eval = eval * 10 + *z - '0';
			z++;
		}
		while(eval >= 64) {
			scale *= 1.0e+64;
			eval -= 64;
		}
		while(eval >= 16) {
			scale *= 1.0e+16;
			eval -= 16;
		}
		while(eval >= 4) {
			scale *= 1.0e+4;
			eval -= 4;
		}
		while(eval >= 1) {
			scale *= 1.0e+1;
			eval -= 1;
		}
		if (esign < 0) {
			v1 /= scale;
		} else {
			v1 *= scale;
		}
	}
	return sign < 0 ? -v1 : v1;
}
