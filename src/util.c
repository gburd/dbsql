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

/*
 * Utility functions for DBSQL
 */

#include "dbsql_config.h"
#include "dbsql_int.h"
#include <ctype.h>
#include <string.h>

/*
 * __dbsql_atoi64 --
 *	Convert a string to a 64-bit signed integer.
 *
 * PUBLIC: int __dbsql_atoi64 __P((const char *, i64 *));
 */
int
__dbsql_atoi64(const char *z, i64 *pNum)
{
	i64 v = 0;
	int neg = 0;
	int c;
	const char *zStart;

	if (z == 0)
		return 0;

	/* Skip leading whitespace */
	while (*z && isspace((unsigned char)*z))
		z++;

	/* Handle sign */
	if (*z == '-') {
		neg = 1;
		z++;
	} else if (*z == '+') {
		z++;
	}

	/* Remember start of digits */
	zStart = z;

	/* Convert digits */
	while ((c = *z) >= '0' && c <= '9') {
		v = v * 10 + (c - '0');
		z++;
	}

	/* Apply sign */
	if (neg)
		v = -v;

	*pNum = v;

	/* Return 1 if valid conversion, 0 if no digits found */
	return (z > zStart) ? 1 : 0;
}

/*
 * __sql_parser_fallback --
 *	Token fallback function for SQL parser.
 *	Returns the fallback token for a given token, or 0 if no fallback.
 *
 * PUBLIC: int __sql_parser_fallback __P((int));
 */
int
__sql_parser_fallback(int iToken)
{
	/* No fallback tokens configured - stub implementation */
	return 0;
}

/*
 * __vdbe_dequote_p3 --
 *	Remove quotes from the P3 parameter of a VDBE instruction.
 *	Modifies the instruction's P3 string in-place if it's quoted.
 *
 * PUBLIC: void __vdbe_dequote_p3 __P((vdbe_t *, int));
 */
void
__vdbe_dequote_p3(vdbe_t *v, int addr)
{
	vdbe_op_t *pOp;
	char *z;

	if (v == 0 || addr < 0 || addr >= v->nOp)
		return;

	pOp = &v->opcodes[addr];
	z = pOp->p3;

	if (z == 0 || z[0] == 0)
		return;

	/* Check if string is quoted with single or double quotes */
	if ((z[0] == '\'' || z[0] == '"') && z[0] == z[strlen(z)-1]) {
		/* Remove surrounding quotes by shifting string */
		int len = strlen(z);
		if (len > 2) {
			memmove(z, z+1, len-2);
			z[len-2] = '\0';
		} else {
			z[0] = '\0';
		}
	}
}
