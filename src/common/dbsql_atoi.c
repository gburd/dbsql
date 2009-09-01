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

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __dbsql_atoi --
 *	Return TRUE if 'str' is a 32-bit signed integer and write
 *	the value of the integer into '*num'.  If 'str' is not an integer
 *	or is an integer that is too large to be expressed with just 32
 *	bits, then return false.
 *
 * PUBLIC: int __dbsql_atoi __P((const char *, int *));
 */
int
__dbsql_atoi(str, num)
	const char *str;
	int *num;
{
	int v = 0;
	int neg;
	int i, c;
	if (*str == '-') {
		neg = 1;
		str++;
	} else if (*str == '+') {
		neg = 0;
		str++;
	} else {
		neg = 0;
	}
	for (i = 0; (c = str[i]) >= '0' && c <= '9'; i++) {
		v = (v * 10) + c - '0';
	}
	*num = neg ? -v : v;
	return (c == 0 && i > 0 &&
		(i < 10 || (i == 10 && memcmp(str,"2147483647", 10) <= 0)));
}
