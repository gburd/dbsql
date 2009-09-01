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
/*
 * Copyright (c) 1990-2004
 *      Sleepycat Software.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Redistributions in any form must be accompanied by information on
 *    how to obtain complete source code for the DB software and any
 *    accompanying software that uses the DB software.  The source code
 *    must either be included in the distribution or be available for no
 *    more than the cost of distribution plus a nominal fee, and must be
 *    freely redistributable under reasonable conditions.  For an
 *    executable file, complete source code means the source code for all
 *    modules it contains.  It does not include source code for modules or
 *    files that typically accompany the major components of the operating
 *    system on which the executable file runs.
 *
 * THIS SOFTWARE IS PROVIDED BY ORACLE CORPORATION ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * NON-INFRINGEMENT, ARE DISCLAIMED.  IN NO EVENT SHALL ORACLE CORPORATION
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) 1990, 1993, 1994, 1995
 * The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Copyright (c) 1995, 1996
 * The President and Fellows of Harvard University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

unsigned char __str_upper_to_lower[] = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
     18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
     36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
     54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 97, 98, 99,100,101,102,103,
    104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,
    122, 91, 92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,
    108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
    126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,
    162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,
    180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,
    198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,
    216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,
    234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,
    252,253,254,255
};

/*
 * __str_append --
 *      Create a string from the 2nd and subsequent arguments (up
 *      to the first NULL argument), store the string in memory
 *      obtained from __dbsql_calloc() and make the pointer indicated
 *      by the 1st argument point to that string.  The 1st argument
 *      must either be NULL or point to memory obtained from
 *      __dbsql_calloc().
 *
 * PUBLIC: void __str_append __P((char **, const char *, ...));
 */
void
#ifdef STDC_HEADERS
__str_append(char **result, const char *fmt, ...)
#else
__str_append(result, fmt, va_alist)
	char **result;
	const char *fmt;
	va_dcl
#endif
{
	va_list ap;
	size_t len;
	const char *tmp;
	char *r;

	if (result == 0)
		return;
	len = strlen(fmt) + 1;
	va_start(ap, fmt);
	while((tmp = va_arg(ap, const char*)) != NULL) {
		len += strlen(tmp);
	}
	va_end(ap);
	__dbsql_free(NULL, *result);
	if (__dbsql_calloc(NULL, 1, len, &r) == ENOMEM)
		return;
	*result = r;
	strcpy(r, fmt);
	r += strlen(r);
	va_start(ap, fmt);
	while((tmp = va_arg(ap, const char*)) != NULL) {
		strcpy(r, tmp);
		r += strlen(r);
	}
	va_end(ap);
}

/*
 * __str_nappend --
 *	Works like __str_append, but each string is now followed by
 *	a length integer which specifies how much of the source string 
 *	to copy (in bytes).  -1 means use the whole string.  The 1st 
 *	argument must either be NULL or point to memory obtained from 
 *	__dbsql_calloc().
 *
 * PUBLIC: void __str_nappend __P((char **, ...));
 */
void
#ifdef STDC_HEADERS
__str_nappend(char **result, ...)
#else
__str_nappend(result, va_alist)
	char **result;
	va_dcl
#endif
{
	va_list ap;
	size_t len;
	const char *tmp;
	char *r;
	int n;

	if (result == 0)
		return;
	len = 0;
	va_start(ap, result);
	while((tmp = va_arg(ap, const char*)) != NULL) {
		n = va_arg(ap, int);
		if (n <= 0)
			n = strlen(tmp);
		len += (size_t)n;
	}
	va_end(ap);
	__dbsql_free(NULL, *result);
	if (__dbsql_calloc(NULL, 1, len + 1, &r) == ENOMEM)
		return;
	*result = r;
	va_start(ap, result);
	while((tmp = va_arg(ap, const char*)) != NULL) {
		n = va_arg(ap, int);
		if (n <= 0)
			n = strlen(tmp);
		strncpy(r, tmp, n);
		r += n;
	}
	*r = 0;
	va_end(ap);
}

/*
 * __str_unquote --
 *	Convert an SQL-style quoted string into a normal string by removing
 *	the quote characters.  The conversion is done in-place.  If the
 *	input does not begin with a quote character, then this routine
 *	is a no-op.  Quotes can be of the form "'a-b-c'" or the MS-Access style
 *	brackets around identifers such as:  "[a-b-c]".  In both cases the
 *	result is "a-b-c".
 *
 * PUBLIC: void __str_unquote __P((char *));
 */
void
__str_unquote(z)
	char *z;
{
	int quote;
	int i, j;

	if (z == NULL)
		return;
	quote = z[0];
	switch(quote) {
	case '\'':  break;
	case '"':   break;
	case '[':   quote = ']';  break;
	default:    return;
	}
	for(i = 1, j = 0; z[i]; i++) {
		if (z[i] == quote) {
			if (z[i + 1] == quote) {
				z[j++] = quote;
				i++;
			} else {
				z[j++] = 0;
				break;
			}
		} else {
			z[j++] = z[i];
		}
	}
}

/*
 * __str_urealloc --
 *	Make a duplicate of a string into memory obtained from
 *	__dbsql_umalloc() Free the original string using __dbsql_free().
 *	This routine is called on all strings that are passed outside of
 *	the library.  That way clients can free the string using
 *	__dbsql_ufree()	rather than having to call __dbsql_free().
 *
 * PUBLIC: int __str_urealloc __P((char **));
 */
int
__str_urealloc(pz)
	char **pz;
{
	int rc = DBSQL_SUCCESS;
	char *new;

	if (pz == 0 || *pz == NULL)
		return;
	
	if (__dbsql_umalloc(NULL, strlen(*pz) + 1, &new) == ENOMEM) {
		rc = ENOMEM;
		__dbsql_free(NULL, *pz);
		*pz = 0;
	}
	strcpy(new, *pz);
	__dbsql_free(NULL, *pz);
	*pz = new;
	return rc;
}

/*
 * __str_is_numeric --
 *	Return TRUE if z is a pure numeric string.  Return FALSE if the
 *	string contains any character which is not part of a number.
 *	An empty string is considered non-numeric.
 *
 * PUBLIC: int __str_is_numeric __P((const char *));
 */
int
__str_is_numeric(z)
	const char *z;
{
	if (*z == '-' || *z == '+')
		z++;
	if (!isdigit(*z)) {
		return 0;
	}
	z++;
	while(isdigit(*z)) {
		z++;
	}
	if (*z == '.') {
		z++;
		if (!isdigit(*z))
			return 0;
		while(isdigit(*z)) {
			z++;
		}
	}
	if (*z == 'e' || *z == 'E') {
		z++;
		if (*z == '+' || *z == '-')
			z++;
		if (!isdigit(*z))
			return 0;
		while(isdigit(*z)) {
			z++;
		}
	}
	return (*z == '\0');
}

#ifdef DBSQL_UTF8_ENCODING

/*
 * Convert the UTF-8 character to which z points into a 31-bit
 * UCS character.  This only works right if z points to a well-formed
 * UTF-8 string.
 */
static int __utf8_to_int(const unsigned char *z) {
	int c;
	static const int init_val[] = {
  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
 15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
 30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
 45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
 60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
 75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
 90,  91,  92,  93,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 104,
105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,   0,   1,   2,
  3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,
 18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,   0,
  1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
  0,   1,   2,   3,   4,   5,   6,   7,   0,   1,   2,   3,   0,   1, 254,
255,
};
	c = init_val[*(z++)];
	while((0xc0 & *z) == 0x80) {
		c = (c<<6) | (0x3f & *(z++));
	}
	return c;
}

/*
 * X is a pointer to the first byte of a UTF-8 character.  Increment
 * X so that it points to the next character.  This only works right
 * if X points to a well-formed UTF-8 string.
 */
#define __next_char(X)  while((0xc0 & *++(X)) == 0x80) {}
#define __char_val(X)   __utf8_to_int(X)

#else /* !defined(DBSQL_UTF8_ENCODING) */
/*
 * For iso8859 encoding, the next character is just the next byte.
 */
#define __next_char(X)  (++(X));
#define __char_val(X)   ((int)*(X))

#endif /* defined(DBSQL_UTF8_ENCODING) */

/*
 * __str_glob_cmp --
 *	Compare two UTF-8 strings for equality where the first string can
 *	potentially be a "glob" expression.  Return true (1) if they
 *	are the same and false (0) if they are different.
 *
 *	Globbing rules:
 *
 *	     '*'       Matches any sequence of zero or more characters.
 *
 *	     '?'       Matches exactly one character.
 *
 *	    [...]      Matches one character from the enclosed list of
 *	               characters.
 *
 *	    [^...]     Matches one character not in the enclosed list.
 *
 *	With the [...] and [^...] matching, a ']' character can be included
 *	in the list by making it the first character after '[' or '^'.  A
 *	range of characters can be specified using '-'.  Example:
 *	"[a-z]" matches any single lower-case letter.  To match a '-', make
 *	it the last character in the list.
 *
 *	This routine is usually quick, but can be N**2 in the worst case.
 *
 *	Hints: to match '*' or '?', put them in "[]".  Like this:
 *
 *	        abc[*]xyz        Matches "abc*xyz" only
 *
 * PUBLIC: int __str_glob_cmp __P((const unsigned char *,
 * PUBLIC:                    const unsigned char *));
 */
int 
__str_glob_cmp(pattern, string)
	const unsigned char *pattern;
	const unsigned char *string;
{
	char c;
	int invert;
	int seen;
	char c2;

	while((c = *pattern) != 0) {
		switch(c) {
		case '*':
			while((c = pattern[1]) == '*' || c == '?') {
				if (c == '?') {
					if (*string == 0)
						return 0;
					__next_char(string);
				}
				pattern++;
			}
			if (c == 0)
				return 1;
			if (c == '[') {
				while(*string &&
				      __str_glob_cmp(&pattern[1],
						     string) == 0) {
					__next_char(string);
				}
				return (*string != 0);
			} else {
				while((c2 = *string) != 0) {
					while(c2 != 0 && c2 != c) {
						c2 = *++string;
					}
					if (c2 == 0)
						return 0;
					if (__str_glob_cmp(&pattern[1],
							   string))
						return 1;
					__next_char(string);
				}
				return 0;
			}
		case '?': {
			if (*string == 0)
				return 0;
			__next_char(string);
			pattern++;
			break;
		}
		case '[': {
			int prior_c = 0;
			seen = 0;
			invert = 0;
			c = __char_val(string);
			if (c == 0)
				return 0;
			c2 = *++pattern;
			if (c2 == '^') {
				invert = 1;
				c2 = *++pattern;
			}
			if (c2 == ']') {
				if (c == ']')
					seen = 1;
				c2 = *++pattern;
			}
			while((c2 = __char_val(pattern)) != 0 && c2 != ']') {
				if (c2 == '-' && pattern[1] != ']' &&
				    pattern[1] != 0 && prior_c > 0) {
					pattern++;
					c2 = __char_val(pattern);
					if (c >= prior_c && c <= c2)
						seen = 1;
					prior_c = 0;
				} else if (c == c2) {
					seen = 1;
					prior_c = c2;
				} else {
					prior_c = c2;
				}
				__next_char(pattern);
			}
			if (c2 == 0 || (seen ^ invert) == 0)
				return 0;
			__next_char(string);
			pattern++;
			break;
		}
		default: {
			if (c != *string )
				return 0;
			pattern++;
			string++;
			break;
		}
		}
	}
	return (*string == 0);
}

/*
 * __str_like_cmp --
 *	Compare two UTF-8 strings for equality using the "LIKE" operator of
 *	SQL.  The '%' character matches any sequence of 0 or more
 *	characters and '_' matches any single character.  Case is
 *	not significant.  This routine is just an adaptation of the
 *	__str_glob_cmp() routine above.
 *
 * PUBLIC: int __str_like_cmp __P((const unsigned char *,
 * PUBLIC:                    const unsigned char *));
 */
int 
__str_like_cmp(pattern, string)
	const unsigned char *pattern;
	const unsigned char *string;
{
	register int c;
	int c2;

	while((c = __str_upper_to_lower[*pattern]) != 0) {
		switch(c) {
		case '%': {
			while((c = pattern[1]) == '%' || c == '_') {
				if (c == '_') {
					if (*string == 0)
						return 0;
					__next_char(string);
				}
				pattern++;
			}
			if (c == 0)
				return 1;
			c = __str_upper_to_lower[c];
			while((c2 = __str_upper_to_lower[*string]) != 0) {
				while(c2 != 0 && c2 != c) {
					c2 = __str_upper_to_lower[*++string];
				}
				if (c2 == 0)
					return 0;
				if (__str_like_cmp(&pattern[1], string))
					return 1;
				__next_char(string);
			}
			return 0;
		}
		case '_': {
			if (*string == 0)
				return 0;
			__next_char(string);
			pattern++;
			break;
		}
		default: {
			if (c != __str_upper_to_lower[*string])
				return 0;
			pattern++;
			string++;
			break;
		}
		}
	}
	return (*string == 0);
}

/*
 * __str_numeric_cmp --
 *	This comparison routine is what we use for comparison operations
 *	between numeric values in an SQL expression.  "Numeric" is a little
 *	bit misleading here.  What we mean is that the strings have a
 *	type of "numeric" from the point of view of SQL.  The strings
 *	do not necessarily contain numbers.  They could contain text.
 *
 *	If the input strings both look like actual numbers then they
 *	compare in numerical order.  Numerical strings are always less 
 *	than non-numeric strings so if one input string looks like a
 *	number and the other does not, then the one that looks like
 *	a number is the smaller.  Non-numeric strings compare in 
 *	lexigraphical order (the same order as strcmp()).
 *
 * PUBLIC: int __str_numeric_cmp __P((const char *, const char *));
 */
int
__str_numeric_cmp(left, right)
	const char *left;
	const char *right;
{
	int result;
	int left_is_num, right_is_num;

	if (left == 0) {
		return -1;
	} else if (right == 0) {
		return 1;
	}
	left_is_num = __str_is_numeric(left);
	right_is_num = __str_is_numeric(right);
	if (left_is_num) {
		if (!right_is_num) {
			result = -1;
		} else {
			double rl, rr;
			rl = __dbsql_atof(left);
			rr = __dbsql_atof(right);
			if (rl < rr){
				result = -1;
			} else if (rl > rr) {
				result = +1;
			} else {
				result = 0;
			}
		}
	} else if (right_is_num) {
		result = +1;
	} else {
		result = strcmp(left, right);
	}
	return result; 
}

/*
 * __str_int_in32b --
 *	The string 'num' represents an integer.  There might be some other
 *	information following the integer too, but that part is ignored.
 *	If the integer that the prefix of 'num' represents will fit in a
 *	32-bit signed integer, return TRUE.  Otherwise return FALSE.
 *
 *	This routine returns FALSE for the string -2147483648 even though that
 *	that number will, in theory fit in a 32-bit integer.  But positive
 *	2147483648 will not fit in 32 bits.  So it seems safer to return
 *	false.
 *
 * PUBLIC: int __str_int_in32b __P((const char *));
 */
int
__str_int_in32b(num)
	const char *num;
{
	int c, i = 0;
	if (*num == '-' || *num == '+')
		num++;
	while ((c = num[i]) >= '0' && c <= '9') {
		i++;
	}
	return (i < 10 || (i == 10 && memcmp(num, "2147483647", 10) <= 0));
}

/*
 * Some powers of 64.  These constants are needed in the
 * __str_real_as_sortable() routine below.
 */
#define _64e3  (64.0 * 64.0 * 64.0)
#define _64e4  (64.0 * 64.0 * 64.0 * 64.0)
#define _64e15 (_64e3 * _64e4 * _64e4 * _64e4)
#define _64e16 (_64e4 * _64e4 * _64e4 * _64e4)
#define _64e63 (_64e15 * _64e16 * _64e16 * _64e16)
#define _64e64 (_64e16 * _64e16 * _64e16 * _64e16)

/*
 * __str_real_as_sortable --
 *	The following procedure converts a double-precision floating point
 *	number into a string.  The resulting string has the property that
 *	two such strings comparied using strcmp() or memcmp() will give the
 *	same results as a numeric comparison of the original floating point
 *	numbers.
 *
 *	This routine is used to generate database keys from floating point
 *	numbers such that the keys sort in the same order as the original
 *	floating point numbers even though the keys are compared using
 *	memcmp().
 *
 *	The calling function should have allocated at least 14 characters
 *	of space for the buffer z[].
 *
 * PUBLIC: void __str_real_as_sortable __P((double, char *));
 */
void
__str_real_as_sortable(r, z)
	double r;
	char *z;
{
	int neg;
	int exp;
	int cnt = 0;

	/*
	 * This array maps integers between 0 and 63 into base-64 digits.
	 * The digits must be chosen such at their ASCII codes are increasing.
	 * This means we can not use the traditional base-64 digit set.
	 */
	static const char digit[] = 
		"0123456789"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"|~";
	if (r < 0.0) {
		neg = 1;
		r = -r;
		*z++ = '-';
	} else {
		neg = 0;
		*z++ = '0';
	}
	exp = 0;

	if (r == 0.0) {
		exp = -1024;
	} else if (r < (0.5 / 64.0)) {
		while(r < 0.5 / _64e64 && exp > -961) {
			r *= _64e64;
			exp -= 64;
		}
		while(r < 0.5 / _64e16 && exp > -1009) {
			r *= _64e16;
			exp -= 16;
		}
		while(r < 0.5 / _64e4 && exp > -1021) {
			r *= _64e4;
			exp -= 4;
		}
		while(r < 0.5 / 64.0 && exp > -1024) {
			r *= 64.0;
			exp -= 1;
		}
	} else if (r >= 0.5) {
		while(r >= 0.5 * _64e63 && exp < 960) {
			r *= 1.0 / _64e64;
			exp += 64;
		}
		while(r >= 0.5 * _64e15 && exp < 1008) {
			r *= 1.0 / _64e16;
			exp += 16;
		}
		while(r >= 0.5 * _64e3 && exp < 1020) {
			r *= 1.0 / _64e4;
			exp += 4;
		}
		while(r >= 0.5 && exp < 1023) {
			r *= 1.0/64.0;
			exp += 1;
		}
	}
	if (neg) {
		exp = -exp;
		r = -r;
	}
	exp += 1024;
	r += 0.5;
	if (exp < 0)
		return;
	if (exp >= 2048 || r >= 1.0) {
		strcpy(z, "~~~~~~~~~~~~");
		return;
	}
	*z++ = digit[(exp >> 6) & 0x3f];
	*z++ = digit[exp & 0x3f];
	while(r > 0.0 && cnt < 10) {
		int d;
		r *= 64.0;
		d = (int)r;
		DBSQL_ASSERT(d >= 0 && d < 64);
		*z++ = digit[d & 0x3f];
		r -= d;
		cnt++;
	}
	*z = 0;
}

/*
 * __str_cmp --
 *	This routine is used for sorting.  Each key is a list of one or more
 *	null-terminated elements.  The list is terminated by two nulls in
 *	a row.  For example, the following text is a key with three elements
 *
 *            Aone\000Dtwo\000Athree\000\000
 *
 *	All elements begin with one of the characters "+-AD" and end with
 *	"\000" with zero or more text elements in between.  Except, NULL
 *	elements consist of the special two-character sequence "N\000".
 *
 *	Both arguments will have the same number of elements.  This routine
 *	returns negative, zero, or positive if the first argument is less
 *	than, equal to, or greater than the first.  (Result is a-b).
 *
 *	Each element begins with one of the characters "+", "-", "A", "D".
 *	This character determines the sort order and collating sequence:
 *
 *	    +      Sort numerically in ascending order
 *	    -      Sort numerically in descending order
 *	    A      Sort as strings in ascending order
 *	    D      Sort as strings in descending order.
 *
 *	For the "+" and "-" sorting, pure numeric strings (strings for which
 *	the __str_is_numeric() function above returns TRUE) always compare
 *	less than strings that are not pure numerics.  Non-numeric strings
 *	compare in memcmp() order.  This is the same sort order as the
 *	__str_numeric_cmp() function above generates.
 *
 *	Elements that begin with 'A' or 'D' compare in memcmp() order
 *	regardless of whether or not they look like a number.
 *
 *	Note that the sort order imposed by the rules above is the same
 *	from the ordering defined by the "<", "<=", ">", and ">=" operators
 *	of expressions and for indices.
 *
 * PUBLIC: int __str_cmp __P((const char *, const char *));
 */
int
__str_cmp(a, b)
	const char *a;
	const char *b;
{
	int res = 0;
	int a_numeric_p, b_numeric_p;
	int dir = 0;

	while(res == 0 && *a && *b) {
		if (a[0] == 'N' || b[0] == 'N') {
			if (a[0] == b[0]) {
				a += 2;
				b += 2;
				continue;
			}
			if (a[0] == 'N') {
				dir = b[0];
				res = -1;
			} else {
				dir = a[0];
				res = +1;
			}
			break;
		}
		DBSQL_ASSERT(a[0] == b[0]);
		if ((dir = a[0]) == 'A' || a[0] == 'D') {
			res = strcmp(&a[1], &b[1]);
			if (res)
				break;
		} else {
			a_numeric_p = __str_is_numeric(&a[1]);
			b_numeric_p = __str_is_numeric(&b[1]);
			if (a_numeric_p) {
				double rA, rB;
				if (!b_numeric_p) {
					res = -1;
					break;
				}
				rA = __dbsql_atof(&a[1]);
				rB = __dbsql_atof(&b[1]);
				if (rA < rB) {
					res = -1;
					break;
				}
				if (rA > rB) {
					res = +1;
					break;
				}
			} else if (b_numeric_p) {
				res = +1;
				break;
			} else {
				res = strcmp(&a[1], &b[1]);
				if (res)
					break;
			}
		}
		a += strlen(&a[1]) + 2;
		b += strlen(&b[1]) + 2;
	}
	if (dir == '-' || dir == 'D')
		res = -res;
	return res;
}
