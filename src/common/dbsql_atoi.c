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
 * $Id: dbsql_atoi.c 7 2007-02-03 13:34:17Z gburd $
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
