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
 * This file contains the C functions that implement various SQL
 * functions of DBSQL.  Some functions (date and time) are implemented
 * separately in cg_date.c.
 *
 * New functions added from SQLite 3.52.0:
 *   - typeof()           Return the type name of an argument
 *   - instr()            Find position of a substring
 *   - replace()          String replacement
 *   - trim/ltrim/rtrim() String trimming
 *   - hex()              Convert value to hexadecimal text
 *   - unhex()            Convert hexadecimal text to blob
 *   - concat()           Concatenate strings
 *   - concat_ws()        Concatenate with separator
 *   - unicode()          Return unicode codepoint of first char
 *   - char()             Build string from unicode codepoints
 *   - sign()             Return -1, 0, or 1
 *   - total()            Like sum() but returns 0.0 for empty set
 *   - group_concat()     Aggregate string concatenation
 *   - Math functions:    ceil, floor, trunc, ln, log, log10, log2,
 *                        exp, pow, mod, sqrt, acos, asin, atan,
 *                        atan2, cos, sin, tan, cosh, sinh, tanh,
 *                        acosh, asinh, atanh, radians, degrees, pi
 */

#include "inc/dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "dbsql_int.h"

/*
 * Mathematical constants used by math functions.
 */
#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif
#ifndef M_LN10
#define M_LN10 2.302585092994045684017991454684364208
#endif
#ifndef M_LN2
#define M_LN2 0.693147180559945309417232121458176568
#endif

/*
 * Hexadecimal digit characters used by __hex_func.
 */
static const char __hex_digits[] = "0123456789ABCDEF";

/* __min_func --
 *	Implementation of the non-aggregate min() function.
 *
 * STATIC: static void __min_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__min_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *best;
	int i;

	if (argc == 0)
		return;
	best = argv[0];
	if (best == 0)
		return;
	for(i = 1; i < argc; i++) {
		if (argv[i] == 0)
			return;
		if (__str_numeric_cmp(argv[i], best) < 0) {
			best = argv[i];
		}
	}
	dbsql_set_result_string(context, best, -1);
}

/*
 * __max_func --
 *	Implementation of the non-aggregate max() function.
 *
 * STATIC: static void __max_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__max_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *best;
	int i;

	if (argc == 0)
		return;
	best = argv[0];
	if (best == 0)
		return;
	for(i = 1; i < argc; i++) {
		if (argv[i] == 0)
			return;
		if (__str_numeric_cmp(argv[i], best) > 0) {
			best = argv[i];
		}
	}
	dbsql_set_result_string(context, best, -1);
}

/*
 * __typeof_func --
 *	Implementation of the typeof() function.  Returns the type
 *	name of the argument as a string: "text", "numeric", or "null".
 *	(SQLite 3.52.0: typeofFunc)
 *
 * STATIC: static void __typeof_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__typeof_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0) {
		dbsql_set_result_string(context, "null", 4);
	} else if (__str_is_numeric(argv[0])) {
		dbsql_set_result_string(context, "numeric", 7);
	} else {
		dbsql_set_result_string(context, "text", 4);
	}
}

/*
 * __length_func --
 *	Implementation of the length() function.
 *
 * STATIC: static void __length_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__length_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *z;
	int len;

	DBSQL_ASSERT(argc == 1);
	z = argv[0];
	if (z == 0)
		return;
#ifdef DBSQL_UTF8_ENCODING
	for(len = 0; *z; z++) {
		if ((0xc0 & *z) != 0x80)
			len++;
	}
#else
	len = strlen(z);
#endif
	dbsql_set_result_int(context, len);
}

/*
 * __abs_func --
 *	Implementation of the abs() function.
 *
 * STATIC: static void __abs_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__abs_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *z;
	DBSQL_ASSERT(argc == 1);
	z = argv[0];
	if (z == 0)
		return;
	if (z[0] == '-' && isdigit(z[1]))
		z++;
	dbsql_set_result_string(context, z, -1);
}

/*
 * __substr_func --
 *	Implementation of the substr() function.
 *
 * STATIC: static void __substr_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__substr_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *z;
#ifdef DBSQL_UTF8_ENCODING
	const char *z2;
	int i;
#endif
	int p1, p2, len;
	DBSQL_ASSERT(argc == 3);
	z = argv[0];
	if (z == 0)
		return;
	p1 = atoi(argv[1] ? argv[1] : 0);
	p2 = atoi(argv[2] ? argv[2] : 0);
#ifdef DBSQL_UTF8_ENCODING
	for(len = 0, z2 = z; *z2; z2++) {
		if ((0xc0 & *z2) != 0x80)
			len++;
	}
#else
	len = strlen(z);
#endif
	if (p1 < 0) {
		p1 += len;
		if (p1 < 0) {
			p2 += p1;
			p1 = 0;
		}
	} else if (p1 > 0) {
		p1--;
	}
	if (p1 + p2 > len) {
		p2 = len - p1;
	}
#ifdef DBSQL_UTF8_ENCODING
	for(i = 0; i < p1 && z[i]; i++) {
		if ((z[i] & 0xc0) == 0x80)
			p1++;
	}
	while(z[i] && (z[i] & 0xc0) == 0x80) {
		i++;
		p1++;
	}
	for(; i < p1 + p2 && z[i]; i++) {
		if ((z[i] & 0xc0) == 0x80)
			p2++;
	}
	while(z[i] && (z[i] & 0xc0) == 0x80) {
		i++;
		p2++;
	}
#endif
	if (p2 < 0)
		p2 = 0;
	dbsql_set_result_string(context, &z[p1], p2);
}

/*
 * __instr_func --
 *	Implementation of the instr(haystack, needle) function.
 *	Returns the 1-based position of needle in haystack, or 0 if
 *	not found.
 *	(SQLite 3.52.0: instrFunc)
 *
 * STATIC: static void __instr_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__instr_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *haystack;
	const char *needle;

	DBSQL_ASSERT(argc == 2);
	haystack = argv[0];
	needle = argv[1];
	if (haystack == 0 || needle == 0)
		return;
	if (needle[0] == 0) {
		dbsql_set_result_int(context, 0);
		return;
	}

#ifdef DBSQL_UTF8_ENCODING
	{
		const char *z = haystack;
		int needle_len = strlen(needle);
		int pos = 1;
		while (*z) {
			if (strncmp(z, needle, needle_len) == 0) {
				dbsql_set_result_int(context, pos);
				return;
			}
			/* Advance one UTF-8 character */
			if ((0xc0 & *z) != 0x80) {
				pos++;
			}
			z++;
		}
	}
#else
	{
		const char *found = strstr(haystack, needle);
		if (found != 0) {
			dbsql_set_result_int(context,
			    (int)(found - haystack) + 1);
			return;
		}
	}
#endif
	dbsql_set_result_int(context, 0);
}

/*
 * __replace_func --
 *	Implementation of the replace(str, from, to) function.
 *	Replace every occurrence of 'from' in 'str' with 'to'.
 *	(SQLite 3.52.0: replaceFunc)
 *
 * STATIC: static void __replace_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__replace_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *str;
	const char *pattern;
	const char *rep;
	char *out;
	int str_len, pat_len, rep_len;
	int out_len, out_alloc;
	int i, j;

	DBSQL_ASSERT(argc == 3);
	str = argv[0];
	pattern = argv[1];
	rep = argv[2];
	if (str == 0 || pattern == 0 || rep == 0)
		return;

	pat_len = strlen(pattern);
	if (pat_len == 0) {
		dbsql_set_result_string(context, str, -1);
		return;
	}

	str_len = strlen(str);
	rep_len = strlen(rep);
	out_alloc = str_len + 1;
	if (__dbsql_calloc(NULL, 1, out_alloc, &out) == ENOMEM)
		return;

	out_len = 0;
	for (i = 0; i <= str_len - pat_len; ) {
		if (strncmp(&str[i], pattern, pat_len) == 0) {
			/* Grow output buffer if needed */
			while (out_len + rep_len + (str_len - i) + 1
			    > out_alloc) {
				char *new_out;
				out_alloc *= 2;
				if (__dbsql_calloc(NULL, 1, out_alloc,
				    &new_out) == ENOMEM) {
					__dbsql_free(NULL, out);
					return;
				}
				memcpy(new_out, out, out_len);
				__dbsql_free(NULL, out);
				out = new_out;
			}
			memcpy(&out[out_len], rep, rep_len);
			out_len += rep_len;
			i += pat_len;
		} else {
			out[out_len++] = str[i++];
		}
	}
	/* Copy remaining characters */
	for (j = i; j < str_len; j++) {
		out[out_len++] = str[j];
	}
	out[out_len] = 0;
	dbsql_set_result_string(context, out, out_len);
	__dbsql_free(NULL, out);
}

/*
 * __trim_func --
 *	Implementation of the trim(), ltrim(), and rtrim() functions.
 *	The user data encodes which side(s) to trim:
 *	  1 = left trim only (ltrim)
 *	  2 = right trim only (rtrim)
 *	  3 = both sides (trim)
 *	With one argument, trims spaces.  With two arguments, the
 *	second argument is the set of characters to trim.
 *	(SQLite 3.52.0: trimFunc)
 *
 * STATIC: static void __trim_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__trim_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *str;
	const char *chars;
	int str_len;
	int flags;
	const char *start;
	const char *end;

	DBSQL_ASSERT(argc == 1 || argc == 2);
	str = argv[0];
	if (str == 0)
		return;

	chars = (argc == 2 && argv[1]) ? argv[1] : " ";
	str_len = strlen(str);
	start = str;
	end = str + str_len;

	/* Decode flags: 1=left, 2=right, 3=both */
	flags = (int)(long)dbsql_user_data(context);

	/* Left trim */
	if (flags & 1) {
		while (start < end && strchr(chars, *start) != 0) {
			start++;
		}
	}

	/* Right trim */
	if (flags & 2) {
		while (end > start && strchr(chars, *(end - 1)) != 0) {
			end--;
		}
	}

	dbsql_set_result_string(context, start, (int)(end - start));
}

/*
 * __hex_func --
 *	Implementation of the hex() function.  Returns the hexadecimal
 *	rendering of the argument string.
 *	(SQLite 3.52.0: hexFunc)
 *
 * STATIC: static void __hex_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__hex_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *str;
	char *out;
	int i, n;

	DBSQL_ASSERT(argc == 1);
	str = argv[0];
	if (str == 0)
		return;

	n = strlen(str);
	if (__dbsql_calloc(NULL, 1, n * 2 + 1, &out) == ENOMEM)
		return;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)str[i];
		out[i * 2] = __hex_digits[(c >> 4) & 0xf];
		out[i * 2 + 1] = __hex_digits[c & 0xf];
	}
	out[n * 2] = 0;
	dbsql_set_result_string(context, out, n * 2);
	__dbsql_free(NULL, out);
}

/*
 * __unhex_func --
 *	Implementation of the unhex() function.  Decodes a hexadecimal
 *	string into the original byte string.  Returns NULL if the
 *	input contains non-hex characters.
 *	(SQLite 3.52.0: unhexFunc)
 *
 * STATIC: static void __unhex_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__unhex_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *hex;
	const char *allow;
	char *out;
	int hex_len, i, j;

	DBSQL_ASSERT(argc == 1 || argc == 2);
	hex = argv[0];
	if (hex == 0)
		return;
	allow = (argc == 2 && argv[1]) ? argv[1] : "";

	hex_len = strlen(hex);
	if (__dbsql_calloc(NULL, 1, (hex_len / 2) + 1, &out) == ENOMEM)
		return;

	j = 0;
	for (i = 0; hex[i]; ) {
		unsigned char c, d;

		/* Skip allowed separator characters */
		while (hex[i] && !isxdigit(hex[i])) {
			if (strchr(allow, hex[i]) == 0) {
				__dbsql_free(NULL, out);
				return; /* NULL result */
			}
			i++;
		}
		if (hex[i] == 0)
			break;

		c = hex[i++];
		if (hex[i] == 0 || !isxdigit(hex[i])) {
			__dbsql_free(NULL, out);
			return; /* NULL result: odd hex digit */
		}
		d = hex[i++];

		c = isdigit(c) ? c - '0' : (toupper(c) - 'A' + 10);
		d = isdigit(d) ? d - '0' : (toupper(d) - 'A' + 10);
		out[j++] = (char)((c << 4) | d);
	}
	out[j] = 0;
	dbsql_set_result_string(context, out, j);
	__dbsql_free(NULL, out);
}

/*
 * __concat_func --
 *	Implementation of the concat(...) function.  Returns the
 *	concatenation of all non-NULL arguments.
 *	(SQLite 3.52.0: concatFunc)
 *
 * STATIC: static void __concat_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__concat_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int total_len;
	int i, j;
	char *out;

	total_len = 0;
	for (i = 0; i < argc; i++) {
		if (argv[i])
			total_len += strlen(argv[i]);
	}

	if (__dbsql_calloc(NULL, 1, total_len + 1, &out) == ENOMEM)
		return;

	j = 0;
	for (i = 0; i < argc; i++) {
		if (argv[i]) {
			int len = strlen(argv[i]);
			memcpy(&out[j], argv[i], len);
			j += len;
		}
	}
	out[j] = 0;
	dbsql_set_result_string(context, out, j);
	__dbsql_free(NULL, out);
}

/*
 * __concat_ws_func --
 *	Implementation of the concat_ws(separator, ...) function.
 *	Returns the concatenation of all non-NULL arguments (starting
 *	from the second) separated by the first argument.
 *	(SQLite 3.52.0: concatwsFunc)
 *
 * STATIC: static void __concat_ws_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__concat_ws_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *sep;
	int sep_len;
	int total_len;
	int i, j;
	int not_first;
	char *out;

	if (argc < 1)
		return;
	sep = argv[0];
	if (sep == 0)
		return;
	sep_len = strlen(sep);

	total_len = 0;
	not_first = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i]) {
			if (not_first)
				total_len += sep_len;
			total_len += strlen(argv[i]);
			not_first = 1;
		}
	}

	if (__dbsql_calloc(NULL, 1, total_len + 1, &out) == ENOMEM)
		return;

	j = 0;
	not_first = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i]) {
			int len = strlen(argv[i]);
			if (not_first) {
				memcpy(&out[j], sep, sep_len);
				j += sep_len;
			}
			memcpy(&out[j], argv[i], len);
			j += len;
			not_first = 1;
		}
	}
	out[j] = 0;
	dbsql_set_result_string(context, out, j);
	__dbsql_free(NULL, out);
}

/*
 * __unicode_func --
 *	Implementation of the unicode() function.  Returns the integer
 *	unicode code-point value for the first character of the input
 *	string.
 *	(SQLite 3.52.0: unicodeFunc)
 *
 * STATIC: static void __unicode_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__unicode_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const unsigned char *z;
	int cp;

	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0)
		return;
	z = (const unsigned char *)argv[0];
	if (z[0] == 0)
		return;

#ifdef DBSQL_UTF8_ENCODING
	if (z[0] < 0x80) {
		cp = z[0];
	} else if ((z[0] & 0xe0) == 0xc0) {
		cp = ((z[0] & 0x1f) << 6) | (z[1] & 0x3f);
	} else if ((z[0] & 0xf0) == 0xe0) {
		cp = ((z[0] & 0x0f) << 12) |
		    ((z[1] & 0x3f) << 6) |
		    (z[2] & 0x3f);
	} else if ((z[0] & 0xf8) == 0xf0) {
		cp = ((z[0] & 0x07) << 18) |
		    ((z[1] & 0x3f) << 12) |
		    ((z[2] & 0x3f) << 6) |
		    (z[3] & 0x3f);
	} else {
		cp = z[0];
	}
#else
	cp = z[0];
#endif
	dbsql_set_result_int(context, cp);
}

/*
 * __char_func --
 *	Implementation of the char() function.  Takes zero or more
 *	integer arguments.  Returns a string where each character is
 *	the unicode character for the corresponding integer argument.
 *	(SQLite 3.52.0: charFunc)
 *
 * STATIC: static void __char_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__char_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char *out;
	int i, j;

	/* Worst case: 4 bytes per UTF-8 character + NUL */
	if (__dbsql_calloc(NULL, 1, argc * 4 + 1, &out) == ENOMEM)
		return;

	j = 0;
	for (i = 0; i < argc; i++) {
		unsigned int cp;
		if (argv[i] == 0)
			continue;
		cp = (unsigned int)atoi(argv[i]);
		if (cp > 0x10ffff)
			cp = 0xfffd;

#ifdef DBSQL_UTF8_ENCODING
		if (cp < 0x80) {
			out[j++] = (char)(cp & 0x7f);
		} else if (cp < 0x800) {
			out[j++] = (char)(0xc0 | ((cp >> 6) & 0x1f));
			out[j++] = (char)(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			out[j++] = (char)(0xe0 | ((cp >> 12) & 0x0f));
			out[j++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[j++] = (char)(0x80 | (cp & 0x3f));
		} else {
			out[j++] = (char)(0xf0 | ((cp >> 18) & 0x07));
			out[j++] = (char)(0x80 | ((cp >> 12) & 0x3f));
			out[j++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[j++] = (char)(0x80 | (cp & 0x3f));
		}
#else
		out[j++] = (char)(cp & 0xff);
#endif
	}
	out[j] = 0;
	dbsql_set_result_string(context, out, j);
	__dbsql_free(NULL, out);
}

/*
 * __sign_func --
 *	Implementation of the sign() function.  Returns -1, 0, or +1
 *	depending on the sign of the numeric argument.
 *	(SQLite 3.52.0: signFunc)
 *
 * STATIC: static void __sign_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__sign_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double val;

	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	val = __dbsql_atof(argv[0]);
	dbsql_set_result_int(context,
	    val < 0.0 ? -1 : val > 0.0 ? 1 : 0);
}

/*
 * __round_func --
 *	Implementation of the round() function.
 *
 * STATIC: static void __round_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__round_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int n;
	double r;
	char buf[100];
	DBSQL_ASSERT(argc == 1 || argc == 2);
	if (argv[0] == 0 || (argc == 2 && argv[1] == 0))
		return;
	n = (argc == 2 ? atoi(argv[1]) : 0);
	if (n > 30)
		n = 30;
	if (n < 0)
		n = 0;
	r = __dbsql_atof(argv[0]);
	sprintf(buf, "%.*f", n, r);
	dbsql_set_result_string(context, buf, -1);
}

#ifdef DBSQL_SOUNDEX
/*
 * __soundex_func --
 *	Compute the soundex encoding of a word.
 *
 * STATIC: static void __soundex_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__soundex_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char result[8];
	const char *in;
	int i, j;
	static const unsigned char code[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
		1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
		0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
		1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
	};
	DBSQL_ASSERT(argc == 1);
	in = argv[0];
	i = 0;
	while(in[i] && !isalpha(in[i])) {
		i++;
	}
	if (in[i]) {
		result[0] = toupper(in[i]);
		for(j = 1; j < 4 && in[i]; i++) {
			int c = code[in[i] & 0x7f];
			if (c > 0) {
				result[j++] = c + '0';
			}
		}
		while(j < 4) {
			result[j++] = '0';
		}
		result[j] = 0;
		dbsql_set_result_string(context, result, 4);
	} else {
		dbsql_set_result_string(context, "?000", 4);
	}
}
#endif

/*
 * __upper_func --
 *	Implementation of the upper() SQL function.
 *
 * STATIC: static void __upper_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__upper_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char *z;
	int i;
	if (argc < 1 || argv[0] == 0)
		return;
	z = dbsql_set_result_string(context, argv[0], -1);
	if (z == 0)
		return;
	for(i = 0; z[i]; i++) {
		if (islower(z[i]))
			z[i] = toupper(z[i]);
	}
}

/*
 * __lower_func --
 *	Implementation of the lower() SQL function.
 *
 * STATIC: static void __lower_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__lower_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char *z;
	int i;
	if (argc < 1 || argv[0] == 0)
		return;
	z = dbsql_set_result_string(context, argv[0], -1);
	if (z == 0)
		return;
	for(i = 0; z[i]; i++) {
		if (isupper(z[i]))
			z[i] = tolower(z[i]);
	}
}

/*
 * __ifnull_func --
 *	Implementation of the IFNULL(), NVL(), and COALESCE() functions.
 *	All three do the same thing.  They return the first non-NULL
 *	argument.
 *
 * STATIC: static void __ifnull_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__ifnull_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int i;
	for(i = 0; i < argc; i++) {
		if (argv[i]) {
			dbsql_set_result_string(context, argv[i], -1);
			break;
		}
	}
}

/*
 * __random_func --
 *	Implementation of random().  Return a random integer.
 *
 * STATIC: static void __random_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__random_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int n;
	static struct drand48_data rand;
	static int first_time = 1;
	if (first_time) {
		first_time = 0;
		srand48_r(1, &rand); /* XXX create a portable rand function */
	}
	rand32_r(&rand, &n);
	dbsql_set_result_int(context, n);
}

/*
 * __last_inserted_rowid_func --
 *	Implementation of the last_inserted_rowid() SQL function.  The return
 *	value is the same as the __api_last_inserted_rowid() API function.
 *
 * STATIC: static void __last_inserted_rowid_func __P((dbsql_func_t *,
 * STATIC:     int, const char **));
 */
static void
__last_inserted_rowid_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL *dbp = dbsql_user_data(context);
	dbsql_set_result_int(context, dbp->rowid(dbp));
}

/*
 * __like_func --
 *	Implementation of the like() SQL function.  This function implements
 *	the build-in LIKE operator.  The first argument to the function is the
 *	string and the second argument is the pattern.  So, the SQL statements:
 *
 *       A LIKE B
 *
 * is implemented as like(A,B).
 *
 * STATIC: static void __like_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__like_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	if (argv[0] == 0 || argv[1] == 0)
		return;
	dbsql_set_result_int(context,
			     __str_like_cmp((const unsigned char*)argv[0],
					    (const unsigned char*)argv[1]));
}

/*
 * __glob_func --
 *	Implementation of the glob() SQL function.  This function implements
 *	the build-in GLOB operator.  The first argument to the function is the
 *	string and the second argument is the pattern.  So, the SQL statements:
 *
 *       A GLOB B
 *
 * is implemented as glob(A,B).
 *
 * STATIC: static void __glob_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__glob_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	if (argv[0] == 0 || argv[1] == 0)
		return;
	dbsql_set_result_int(context,
			     __str_glob_cmp((const unsigned char*)argv[0],
					    (const unsigned char*)argv[1]));
}

/*
 * __nullif_func --
 *	Implementation of the NULLIF(x,y) function.  The result is the first
 *	argument if the arguments are different.  The result is NULL if the
 *	arguments are equal to each other.
 *
 * STATIC: static void __nullif_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__nullif_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	if (argv[0] != 0 && __str_numeric_cmp(argv[0], argv[1]) != 0) {
		dbsql_set_result_string(context, argv[0], -1);
	}
}

/*
 * __version_func --
 *	Implementation of the VERSION(*) function.  The result is the version
 *	of the library.
 *
 * STATIC: static void __version_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__version_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int major, minor, patch;
	dbsql_set_result_string(context,
				dbsql_version(&major, &minor, &patch), -1);
}

/*
 * __quote_func --
 *	Implementation of the QUOTE() function.  This function takes a single
 *	argument.  If the argument is numeric, the return value is the same as
 *	the argument.  If the argument is NULL, the return value is the string
 *	"NULL".  Otherwise, the argument is enclosed in single quotes with
 *	single-quote escapes.
 *
 * STATIC: static void __quote_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__quote_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	if (argc < 1)
		return;
	if (argv[0] == 0) {
		dbsql_set_result_string(context, "NULL", 4);
	} else if (__str_is_numeric(argv[0])) {
		dbsql_set_result_string(context, argv[0], -1);
	} else {
		int i,j,n;
		char *z;
		for(i = n = 0; argv[0][i]; i++) {
			if (argv[0][i] == '\'')
				n++;
		}
		if (__dbsql_calloc(NULL, 1, i + n + 3, &z) == ENOMEM)
			return;
		z[0] = '\'';
		for(i = 0, j = 1; argv[0][i]; i++) {
			z[j++] = argv[0][i];
			if (argv[0][i] == '\'') {
				z[j++] = '\'';
			}
		}
		z[j++] = '\'';
		z[j] = 0;
		dbsql_set_result_string(context, z, j);
		__dbsql_free(NULL, z);
	}
}

/*
 * ---------------------------------------------------------------
 * Math functions (SQLite 3.52.0 SQLITE_ENABLE_MATH_FUNCTIONS)
 * ---------------------------------------------------------------
 */

#ifndef DBSQL_OMIT_MATH_FUNCTIONS

/*
 * __ceil_func --
 *	Implementation of ceil() and ceiling() functions.
 *	(SQLite 3.52.0: ceilingFunc with xCeil)
 *
 * STATIC: static void __ceil_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__ceil_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	dbsql_set_result_double(context, ceil(__dbsql_atof(argv[0])));
}

/*
 * __floor_func --
 *	Implementation of the floor() function.
 *	(SQLite 3.52.0: ceilingFunc with xFloor)
 *
 * STATIC: static void __floor_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__floor_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	dbsql_set_result_double(context, floor(__dbsql_atof(argv[0])));
}

/*
 * __trunc_func --
 *	Implementation of the trunc() function.
 *	(SQLite 3.52.0: ceilingFunc with trunc)
 *
 * STATIC: static void __trunc_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__trunc_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	dbsql_set_result_double(context, trunc(__dbsql_atof(argv[0])));
}

/*
 * __ln_func --
 *	Implementation of the ln() function (natural logarithm).
 *	(SQLite 3.52.0: logFunc)
 *
 * STATIC: static void __ln_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__ln_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double x;
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	x = __dbsql_atof(argv[0]);
	if (x <= 0.0)
		return;
	dbsql_set_result_double(context, log(x));
}

/*
 * __log_func --
 *	Implementation of log() and log10() functions.
 *	With 1 argument: log base 10 (same as log10).
 *	With 2 arguments: log(base, x) = log(x)/log(base).
 *	(SQLite 3.52.0: logFunc)
 *
 * STATIC: static void __log_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__log_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double x;
	DBSQL_ASSERT(argc == 1 || argc == 2);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	x = __dbsql_atof(argv[0]);
	if (x <= 0.0)
		return;

	if (argc == 2) {
		double b, val;
		if (argv[1] == 0 || !__str_is_numeric(argv[1]))
			return;
		b = log(x);
		if (b <= 0.0)
			return;
		val = __dbsql_atof(argv[1]);
		if (val <= 0.0)
			return;
		dbsql_set_result_double(context, log(val) / b);
	} else {
		dbsql_set_result_double(context, log10(x));
	}
}

/*
 * __log2_func --
 *	Implementation of the log2() function.
 *	(SQLite 3.52.0: logFunc with user_data==2)
 *
 * STATIC: static void __log2_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__log2_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double x;
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	x = __dbsql_atof(argv[0]);
	if (x <= 0.0)
		return;
	dbsql_set_result_double(context, log(x) / M_LN2);
}

/*
 * __math1_func --
 *	Generic implementation of 1-argument math functions using a
 *	function pointer stored in user data.
 *	Used for: exp, sqrt, acos, asin, atan, cos, sin, tan,
 *	          cosh, sinh, tanh, acosh, asinh, atanh
 *	(SQLite 3.52.0: math1Func)
 *
 * STATIC: static void __math1_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__math1_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double val, ans;
	double (*fn)(double);
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	val = __dbsql_atof(argv[0]);
	fn = (double(*)(double))dbsql_user_data(context);
	ans = fn(val);
	dbsql_set_result_double(context, ans);
}

/*
 * __math2_func --
 *	Generic implementation of 2-argument math functions using a
 *	function pointer stored in user data.
 *	Used for: pow/power, mod (fmod), atan2
 *	(SQLite 3.52.0: math2Func)
 *
 * STATIC: static void __math2_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__math2_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	double v0, v1, ans;
	double (*fn)(double, double);
	DBSQL_ASSERT(argc == 2);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	if (argv[1] == 0 || !__str_is_numeric(argv[1]))
		return;
	v0 = __dbsql_atof(argv[0]);
	v1 = __dbsql_atof(argv[1]);
	fn = (double(*)(double, double))dbsql_user_data(context);
	ans = fn(v0, v1);
	dbsql_set_result_double(context, ans);
}

/*
 * __pi_func --
 *	Implementation of the pi() function.
 *	(SQLite 3.52.0: piFunc)
 *
 * STATIC: static void __pi_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__pi_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 0);
	dbsql_set_result_double(context, M_PI);
}

/*
 * __degrees_func --
 *	Implementation of degrees() - convert radians to degrees.
 *	(SQLite 3.52.0: math1Func with radToDeg)
 *
 * STATIC: static void __degrees_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__degrees_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	dbsql_set_result_double(context,
	    __dbsql_atof(argv[0]) * (180.0 / M_PI));
}

/*
 * __radians_func --
 *	Implementation of radians() - convert degrees to radians.
 *	(SQLite 3.52.0: math1Func with degToRad)
 *
 * STATIC: static void __radians_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__radians_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL_ASSERT(argc == 1);
	if (argv[0] == 0 || !__str_is_numeric(argv[0]))
		return;
	dbsql_set_result_double(context,
	    __dbsql_atof(argv[0]) * (M_PI / 180.0));
}

#endif /* !DBSQL_OMIT_MATH_FUNCTIONS */

/*
 * ---------------------------------------------------------------
 * Aggregate functions
 * ---------------------------------------------------------------
 */

/*
 * An instance of the following structure holds the context of a
 * sum() or avg() aggregate computation.
 */
typedef struct {
	double sum;     /* Sum of terms */
	int cnt;        /* Number of elements summed */
} sum_ctx_t;

/*
 * __sum_step --
 *	Routines used to compute the sum or average.
 *
 * STATIC: static void __sum_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__sum_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	sum_ctx_t *p;
	if (argc < 1)
		return;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && argv[0]) {
		p->sum += __dbsql_atof(argv[0]);
		p->cnt++;
	}
}

/*
 * __sum_finalize --
 *	Finalize function for sum().  Returns NULL if no rows.
 *
 * STATIC: static void __sum_finalize __P((dbsql_func_t *));
 */
static void
__sum_finalize(dbsql_func_t* context)
{
	sum_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	dbsql_set_result_double(context, p ? p->sum : 0.0);
}

/*
 * __total_finalize --
 *	Finalize function for total().  Like sum() but always returns
 *	a float and returns 0.0 (not NULL) when there are no rows.
 *	(SQLite 3.52.0: totalFinalize)
 *
 * STATIC: static void __total_finalize __P((dbsql_func_t *));
 */
static void
__total_finalize(dbsql_func_t* context)
{
	sum_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	dbsql_set_result_double(context, p ? p->sum : 0.0);
}

/*
 * __avg_finalize --
 *	Finalize function for avg().
 *
 * STATIC: static void __avg_finalize __P((dbsql_func_t *));
 */
static void
__avg_finalize(dbsql_func_t* context)
{
	sum_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && p->cnt > 0){
		dbsql_set_result_double(context, p->sum / (double)p->cnt);
	}
}

/*
 * An instance of the following structure holds the context of a
 * variance or standard deviation computation.
 */
typedef struct {
	double sum;     /* Sum of terms */
	double sum2;    /* Sum of the squares of terms */
	int cnt;        /* Number of terms counted */
} std_dev_ctx_t;

/*
 * __std_dev_step --
 *	Routines used to compute the standard deviation as an aggregate.
 *
 * STATIC: static void __std_dev_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__std_dev_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	std_dev_ctx_t *p;
	double x;
	if (argc < 1)
		return;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && argv[0]) {
		x = __dbsql_atof(argv[0]);
		p->sum += x;
		p->sum2 += x*x;
		p->cnt++;
	}
}

/*
 * __std_dev_finalize --
 *
 * STATIC: static void __std_dev_finalize __P((dbsql_func_t *));
 */
static void
__std_dev_finalize(dbsql_func_t* context)
{
	double rN = dbsql_aggregate_count(context);
	std_dev_ctx_t *p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && p->cnt > 1) {
		double rCnt = p->cnt;
		dbsql_set_result_double(context,
		      sqrt((p->sum2 - p->sum * p->sum / rCnt)
		      / (rCnt - 1.0)));
	}
}

/*
 * The following structure keeps track of state information for the
 * count() aggregate function.
 */
typedef struct {
	int n;
} count_ctx_t;

/*
 * __count_step --
 *	Routines to implement the count() aggregate function.
 *
 * STATIC: static void __count_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__count_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	count_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if ((argc == 0 || argv[0]) && p) {
		p->n++;
	}
}

/*
 * __count_finalize --
 *
 * STATIC: static void __count_finalize __P((dbsql_func_t *));
 */
static void
__count_finalize(dbsql_func_t* context)
{
	count_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	dbsql_set_result_int(context, p ? p->n : 0);
}

/*
 * This function tracks state information for the min() and max()
 * aggregate functions.
 */
typedef struct {
	char *z;         /* The best so far */
	char buf[28];   /* Space that can be used for storage */
} min_max_ctx_t;

/*
 * __min_step --
 *	Routine to implement min() aggregate function.
 *
 * STATIC: static void __min_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__min_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	min_max_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p == 0 || argc < 1 || argv[0] == 0)
		return;
	if (p->z == 0 || __str_numeric_cmp(argv[0], p->z) < 0) {
		int len;
		if (!p->buf[0]) {
			__dbsql_free(NULL, p->z);
		}
		len = strlen(argv[0]);
		if (len < sizeof(p->buf) - 1) {
			p->z = &p->buf[1];
			p->buf[0] = 1;
		} else {
			p->buf[0] = 0;
			if (__dbsql_calloc(NULL, 1, len + 1, &p->z)
			    == ENOMEM)
				return;
		}
		strcpy(p->z, argv[0]);
	}
}

/*
 * __max_step --
 *	Routine to implement max() aggregate function.
 *
 * STATIC: static void __max_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__max_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	min_max_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p == 0 || argc < 1 || argv[0] == 0)
		return;
	if (p->z == 0 || __str_numeric_cmp(argv[0], p->z) > 0) {
		int len;
		if (!p->buf[0]) {
			__dbsql_free(NULL, p->z);
		}
		len = strlen(argv[0]);
		if (len < sizeof(p->buf) - 1) {
			p->z = &p->buf[1];
			p->buf[0] = 1;
		} else {
			p->buf[0] = 0;
			if (__dbsql_calloc(NULL, 1, len + 1, &p->z)
			    == ENOMEM)
				return;
		}
		strcpy(p->z, argv[0]);
	}
}

/*
 * __min_max_finalize --
 *
 * STATIC: static void __min_max_finalize __P((dbsql_func_t *));
 */
static void
__min_max_finalize(dbsql_func_t* context)
{
	min_max_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && p->z) {
		dbsql_set_result_string(context, p->z, strlen(p->z));
	}
	if (p && !p->buf[0]) {
		__dbsql_free(NULL, p->z);
	}
}

/*
 * Context structure for the group_concat() aggregate function.
 * (SQLite 3.52.0: GroupConcatCtx)
 */
typedef struct {
	char *buf;       /* Accumulated result string */
	int len;         /* Current length of buf content */
	int alloc;       /* Allocated size of buf */
} group_concat_ctx_t;

/*
 * __group_concat_step --
 *	Step function for group_concat() aggregate.
 *	group_concat(EXPR) uses "," as separator.
 *	group_concat(EXPR, SEP) uses SEP as separator.
 *	(SQLite 3.52.0: groupConcatStep)
 *
 * STATIC: static void __group_concat_step __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__group_concat_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	group_concat_ctx_t *p;
	const char *val;
	const char *sep;
	int val_len, sep_len, needed;

	DBSQL_ASSERT(argc == 1 || argc == 2);
	if (argv[0] == 0)
		return;

	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p == 0)
		return;

	val = argv[0];
	val_len = strlen(val);

	/* Determine separator */
	if (p->len > 0) {
		if (argc == 2 && argv[1]) {
			sep = argv[1];
			sep_len = strlen(sep);
		} else {
			sep = ",";
			sep_len = 1;
		}
	} else {
		sep = "";
		sep_len = 0;
	}

	needed = p->len + sep_len + val_len + 1;
	if (needed > p->alloc) {
		char *new_buf;
		int new_alloc = (needed > p->alloc * 2)
		    ? needed + 64 : p->alloc * 2;
		if (p->buf == 0) {
			if (__dbsql_calloc(NULL, 1, new_alloc,
			    &new_buf) == ENOMEM)
				return;
		} else {
			if (__dbsql_calloc(NULL, 1, new_alloc,
			    &new_buf) == ENOMEM)
				return;
			memcpy(new_buf, p->buf, p->len);
			__dbsql_free(NULL, p->buf);
		}
		p->buf = new_buf;
		p->alloc = new_alloc;
	}

	if (sep_len > 0) {
		memcpy(&p->buf[p->len], sep, sep_len);
		p->len += sep_len;
	}
	memcpy(&p->buf[p->len], val, val_len);
	p->len += val_len;
	p->buf[p->len] = 0;
}

/*
 * __group_concat_finalize --
 *	Finalize function for group_concat().
 *	(SQLite 3.52.0: groupConcatFinalize)
 *
 * STATIC: static void __group_concat_finalize __P((dbsql_func_t *));
 */
static void
__group_concat_finalize(dbsql_func_t* context)
{
	group_concat_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && p->buf) {
		dbsql_set_result_string(context, p->buf, p->len);
		__dbsql_free(NULL, p->buf);
	}
}

/*
 * __register_builtin_funcs --
 *	This function registered all of the above C functions as SQL
 *	functions.  This should be the only routine in this file with
 *	external linkage.
 *
 * PUBLIC: void __register_builtin_funcs __P((DBSQL *));
 */
void
__register_builtin_funcs(DBSQL* dbp)
{
	static struct {
		char *name;
		int num_arg;
		int data_type;
		void (*func)(dbsql_func_t*,int,const char**);
	} funcs[] = {
		/* Original DBSQL functions */
		{ "min",       -1, DBSQL_ARGS,    __min_func    },
		{ "min",        0, 0,              0             },
		{ "max",       -1, DBSQL_ARGS,    __max_func    },
		{ "max",        0, 0,              0             },
		{ "length",     1, DBSQL_NUMERIC, __length_func },
		{ "substr",     3, DBSQL_TEXT,    __substr_func },
		{ "abs",        1, DBSQL_NUMERIC, __abs_func    },
		{ "round",      1, DBSQL_NUMERIC, __round_func  },
		{ "round",      2, DBSQL_NUMERIC, __round_func  },
		{ "upper",      1, DBSQL_TEXT,    __upper_func  },
		{ "lower",      1, DBSQL_TEXT,    __lower_func  },
		{ "coalesce",  -1, DBSQL_ARGS,    __ifnull_func },
		{ "coalesce",   0, 0,              0             },
		{ "coalesce",   1, 0,              0             },
		{ "ifnull",     2, DBSQL_ARGS,    __ifnull_func },
		{ "random",    -1, DBSQL_NUMERIC, __random_func },
		{ "like",       2, DBSQL_NUMERIC, __like_func   },
		{ "glob",       2, DBSQL_NUMERIC, __glob_func   },
		{ "nullif",     2, DBSQL_ARGS,    __nullif_func },
		{ "dbsql_get_version",0,DBSQL_TEXT, __version_func},
		{ "quote",      1, DBSQL_ARGS,    __quote_func  },
#ifdef DBSQL_SOUNDEX
		{ "soundex",    1, DBSQL_TEXT,    __soundex_func},
#endif
		/* New functions from SQLite 3.52.0 */
		{ "typeof",     1, DBSQL_TEXT,    __typeof_func },
		{ "instr",      2, DBSQL_NUMERIC, __instr_func  },
		{ "replace",    3, DBSQL_TEXT,    __replace_func},
		{ "hex",        1, DBSQL_TEXT,    __hex_func    },
		{ "unhex",      1, DBSQL_TEXT,    __unhex_func  },
		{ "unhex",      2, DBSQL_TEXT,    __unhex_func  },
		{ "concat",    -1, DBSQL_TEXT,    __concat_func },
		{ "concat_ws", -1, DBSQL_TEXT,
		    __concat_ws_func },
		{ "unicode",    1, DBSQL_NUMERIC, __unicode_func},
		{ "char",      -1, DBSQL_TEXT,    __char_func   },
		{ "sign",       1, DBSQL_NUMERIC, __sign_func   },
		{ "substring",  3, DBSQL_TEXT,    __substr_func },
#ifndef DBSQL_OMIT_MATH_FUNCTIONS
		/* Math functions */
		{ "ceil",       1, DBSQL_NUMERIC, __ceil_func   },
		{ "ceiling",    1, DBSQL_NUMERIC, __ceil_func   },
		{ "floor",      1, DBSQL_NUMERIC, __floor_func  },
		{ "trunc",      1, DBSQL_NUMERIC, __trunc_func  },
		{ "ln",         1, DBSQL_NUMERIC, __ln_func     },
		{ "log",        1, DBSQL_NUMERIC, __log_func    },
		{ "log",        2, DBSQL_NUMERIC, __log_func    },
		{ "log10",      1, DBSQL_NUMERIC, __log_func    },
		{ "log2",       1, DBSQL_NUMERIC, __log2_func   },
		{ "pi",         0, DBSQL_NUMERIC, __pi_func     },
		{ "degrees",    1, DBSQL_NUMERIC, __degrees_func},
		{ "radians",    1, DBSQL_NUMERIC,
		    __radians_func},
#endif
	};
	/* Trim functions need user data for flags */
	static struct {
		char *name;
		int num_arg;
		int data_type;
		void (*func)(dbsql_func_t*,int,const char**);
		int flags;
	} trim_funcs[] = {
		{ "ltrim",  1, DBSQL_TEXT, __trim_func, 1 },
		{ "ltrim",  2, DBSQL_TEXT, __trim_func, 1 },
		{ "rtrim",  1, DBSQL_TEXT, __trim_func, 2 },
		{ "rtrim",  2, DBSQL_TEXT, __trim_func, 2 },
		{ "trim",   1, DBSQL_TEXT, __trim_func, 3 },
		{ "trim",   2, DBSQL_TEXT, __trim_func, 3 },
	};
#ifndef DBSQL_OMIT_MATH_FUNCTIONS
	/*
	 * Math functions that use a function pointer as user data.
	 */
	static struct {
		char *name;
		int num_arg;
		void (*func)(dbsql_func_t*,int,const char**);
		double (*math_fn)(double);
	} math1_funcs[] = {
		{ "exp",    1, __math1_func, exp   },
		{ "sqrt",   1, __math1_func, sqrt  },
		{ "acos",   1, __math1_func, acos  },
		{ "asin",   1, __math1_func, asin  },
		{ "atan",   1, __math1_func, atan  },
		{ "cos",    1, __math1_func, cos   },
		{ "sin",    1, __math1_func, sin   },
		{ "tan",    1, __math1_func, tan   },
		{ "cosh",   1, __math1_func, cosh  },
		{ "sinh",   1, __math1_func, sinh  },
		{ "tanh",   1, __math1_func, tanh  },
#ifdef _GNU_SOURCE
		{ "acosh",  1, __math1_func, acosh },
		{ "asinh",  1, __math1_func, asinh },
		{ "atanh",  1, __math1_func, atanh },
#endif
	};
	static struct {
		char *name;
		int num_arg;
		void (*func)(dbsql_func_t*,int,const char**);
		double (*math_fn)(double, double);
	} math2_funcs[] = {
		{ "pow",    2, __math2_func, pow   },
		{ "power",  2, __math2_func, pow   },
		{ "mod",    2, __math2_func, fmod  },
		{ "atan2",  2, __math2_func, atan2 },
	};
#endif /* !DBSQL_OMIT_MATH_FUNCTIONS */

	static struct {
		char *name;
		int num_arg;
		int data_type;
		void (*step)(dbsql_func_t*,int,const char**);
		void (*finalize)(dbsql_func_t*);
	} aggs[] = {
	{ "min",    1, 0,
	    __min_step,      __min_max_finalize },
	{ "max",    1, 0,
	    __max_step,      __min_max_finalize },
	{ "sum",    1, DBSQL_NUMERIC,
	    __sum_step,      __sum_finalize     },
	{ "total",  1, DBSQL_NUMERIC,
	    __sum_step,      __total_finalize   },
	{ "avg",    1, DBSQL_NUMERIC,
	    __sum_step,      __avg_finalize     },
	{ "count",  0, DBSQL_NUMERIC,
	    __count_step,    __count_finalize   },
	{ "count",  1, DBSQL_NUMERIC,
	    __count_step,    __count_finalize   },
	{ "stddev", 1, DBSQL_NUMERIC,
	    __std_dev_step,  __std_dev_finalize },
	{ "group_concat", 1, DBSQL_TEXT,
	    __group_concat_step, __group_concat_finalize },
	{ "group_concat", 2, DBSQL_TEXT,
	    __group_concat_step, __group_concat_finalize },
	};
	int i;

	for(i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
		dbp->create_function(dbp, funcs[i].name, funcs[i].num_arg,
				     DBSQL_UTF8_ENCODED, NULL, funcs[i].func,
				     NULL, NULL);
		if (funcs[i].func) {
			dbp->func_return_type(dbp, funcs[i].name,
					      funcs[i].data_type);
		}
	}

	/* Register trim functions with flags as user data */
	for(i = 0; i < sizeof(trim_funcs) / sizeof(trim_funcs[0]); i++) {
		dbp->create_function(dbp, trim_funcs[i].name,
				     trim_funcs[i].num_arg,
				     DBSQL_UTF8_ENCODED,
				     (void*)(long)trim_funcs[i].flags,
				     trim_funcs[i].func, NULL, NULL);
		dbp->func_return_type(dbp, trim_funcs[i].name,
				      trim_funcs[i].data_type);
	}

#ifndef DBSQL_OMIT_MATH_FUNCTIONS
	/* Register 1-argument math functions with fn ptr as user data */
	for(i = 0; i < sizeof(math1_funcs) / sizeof(math1_funcs[0]);
	    i++) {
		dbp->create_function(dbp, math1_funcs[i].name,
				     math1_funcs[i].num_arg,
				     DBSQL_UTF8_ENCODED,
				     (void*)math1_funcs[i].math_fn,
				     math1_funcs[i].func, NULL, NULL);
		dbp->func_return_type(dbp, math1_funcs[i].name,
				      DBSQL_NUMERIC);
	}

	/* Register 2-argument math functions with fn ptr as user data */
	for(i = 0; i < sizeof(math2_funcs) / sizeof(math2_funcs[0]);
	    i++) {
		dbp->create_function(dbp, math2_funcs[i].name,
				     math2_funcs[i].num_arg,
				     DBSQL_UTF8_ENCODED,
				     (void*)math2_funcs[i].math_fn,
				     math2_funcs[i].func, NULL, NULL);
		dbp->func_return_type(dbp, math2_funcs[i].name,
				      DBSQL_NUMERIC);
	}
#endif /* !DBSQL_OMIT_MATH_FUNCTIONS */

	dbp->create_function(dbp, "last_inserted_rowid", 0,
			     DBSQL_UTF8_ENCODED, dbp,
			     __last_inserted_rowid_func, NULL, NULL);
	dbp->func_return_type(dbp, "last_inserted_rowid", DBSQL_NUMERIC);

	for(i = 0; i < sizeof(aggs) / sizeof(aggs[0]); i++) {
		dbp->create_function(dbp, aggs[i].name, aggs[i].num_arg,
				     DBSQL_UTF8_ENCODED, NULL,
				     NULL, aggs[i].step, aggs[i].finalize);
		dbp->func_return_type(dbp, aggs[i].name, aggs[i].data_type);
	}

	__register_datetime_funcs(dbp);
}
