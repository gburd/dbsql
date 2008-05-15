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
