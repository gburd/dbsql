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
 * The following is an printf with some extra features (as well as some
 * missing features).  A few extensions to the formatting notation are
 * supported:
 *    *  The "=" flag (similar to "-") causes the output to be
 *       be centered in the appropriately sized field.
 *
 *    *  The %b field outputs an integer in binary notation.
 *
 *    *  The %c field now accepts a precision.  The character output
 *       is repeated by the number of times the precision specifies.
 *
 *    *  The %' field works like %c, but takes as its character the
 *       next character of the format string, instead of the next
 *       argument.  For example,  printf("%.78'-")  prints 78 minus
 *       signs, the same as  printf("%.78c",'-').
 *
 * When compiled using GCC on a SPARC, this version of printf is
 * faster than the library printf for SUN OS 4.1.  Also, all functions
 * are fully reentrant.
 *
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * Conversion types fall into various categories as defined by the
 * following enumeration.
 */
#define etRADIX       1 /* Integer types.  %d, %x, %o, and so forth */
#define etFLOAT       2 /* Floating point.  %f */
#define etEXP         3 /* Exponentional notation. %e and %E */
#define etGENERIC     4 /* Floating or exponential, depending on exponent. %g*/
#define etSIZE        5 /* Return number of characters processed so far. %n */
#define etSTRING      6 /* Strings. %s */
#define etDYNSTRING   7 /* Dynamically allocated strings. %z */
#define etPERCENT     8 /* Percent symbol. %% */
#define etCHARX       9 /* Characters. %c */
#define etERROR      10 /* Used to indicate no such conversion type */
/* The rest are extensions, not normally found in printf() */
#define etCHARLIT    11 /* Literal characters.  %' */
#define etSQLESCAPE  12 /* Strings with '\'' doubled.  %q */
#define etSQLESCAPE2 13 /* Strings with '\'' doubled and enclosed in '',
                          NULL pointers replaced by SQL NULL.  %Q */

/*
 * Each builtin conversion character (ex: the 'd' in "%d") is described
 * by an instance of the following structure
 */
typedef struct et_info {
	char   fmttype;          /* The format field code letter */
	u_int8_t base;           /* The base for radix conversion */
	u_int8_t flags;          /* One or more of FLAG_ constants below */
#define FLAG_SIGNED  1           /* True if the value to convert is signed */
#define FLAG_INTERN  2           /* True if for internal use only */
	u_int8_t type;           /* Conversion paradigm */
	char   *charset;         /* The character set for conversion */
	char   *prefix;          /* Prefix on non-zero values in alt format */
} et_info_t;

/*
 * The following table is searched linearly, so it is good to put the
 * most frequently used conversion types first.
 */
static et_info_t fmtinfo[] = {
  {  'd', 10, 1, etRADIX,      "0123456789",        0    },
  {  's',  0, 0, etSTRING,     0,                   0    },
  {  'z',  0, 2, etDYNSTRING,  0,                   0    },
  {  'q',  0, 0, etSQLESCAPE,  0,                   0    },
  {  'Q',  0, 0, etSQLESCAPE2, 0,                   0    },
  {  'c',  0, 0, etCHARX,      0,                   0    },
  {  'o',  8, 0, etRADIX,      "01234567",         "0"   },
  {  'u', 10, 0, etRADIX,      "0123456789",        0    },
  {  'x', 16, 0, etRADIX,      "0123456789abcdef", "x0"  },
  {  'X', 16, 0, etRADIX,      "0123456789ABCDEF", "X0"  },
  {  'f',  0, 1, etFLOAT,      0,                   0    },
  {  'e',  0, 1, etEXP,        "e",                 0    },
  {  'E',  0, 1, etEXP,        "E",                 0    },
  {  'g',  0, 1, etGENERIC,    "e",                 0    },
  {  'G',  0, 1, etGENERIC,    "E",                 0    },
  {  'i', 10, 1, etRADIX,      "0123456789",        0    },
  {  'n',  0, 0, etSIZE,       0,                   0    },
  {  '%',  0, 0, etPERCENT,    0,                   0    },
  {  'p', 10, 0, etRADIX,      "0123456789",        0    },
};
#define etNINFO  (sizeof(fmtinfo) / sizeof(fmtinfo[0]))

/*
 * If NOFLOATINGPOINT is defined, then none of the floating point
 * conversions will work.
 */
#ifndef etNOFLOATINGPOINT
/*
 * et_get_digit --
 *	"*val" is a double such that 0.1 <= *val < 10.0
 *	Return the ascii code for the leading digit of *val, then
 *	multiply "*val" by 10.0 to renormalize.
 *
 *	Example:
 *	    input:     *val = 3.14159
 *	    output:    *val = 1.4159    function return = '3'
 *
 *	The counter *cnt is incremented each time.  After counter exceeds
 *	16 (the number of significant digits in a 64-bit float) '0' is
 *	always returned.
 */
static int
et_getdigit(val, cnt)
	long_double_t *val;
	int *cnt;
{
	int digit;
	long_double_t d;
	if ((*cnt)++ >= 16 )
		return '0';
	digit = (int)*val;
	d = digit;
	digit += '0';
	*val = (*val - d) * 10.0;
	return digit;
}
#endif

#define etBUFSIZE 1024  /* Size of the output buffer */

/*
 * __et_printf --
 *	The root printf-like program used by all variations implemented herein.
 *
 *	INPUTS:
 *	  func   This is a pointer to a function taking three arguments
 *	           1. A pointer to anything.  Same as the "arg" parameter.
 *	           2. A pointer to the list of characters to be output
 *	              (Note, this list is NOT null terminated.)
 *	           3. An integer number of characters to be output.
 *	              (Note: This number might be zero.)
 *
 *	  arg    This is the pointer to anything which will be passed as the
 *	         first argument to "func".  Use it for whatever you like.
 *
 *	  fmt    This is the format string, as in the usual print.
 *
 *	  ap     This is a pointer to a list of arguments.  Same as in
 *	         vfprint.
 *
 *	OUTPUTS:
 *	         The return value is the total number of characters sent to
 *	         the function "func".  Returns -1 on a error.
 *
 *	NOTE: The order in which automatic variables are declared below
 *	seems to make a big difference in determining how fast this function
 *	runs.
 */
static int
__et_printf(dbp, func, arg, fmt, ap)
	DBSQL *dbp;
	void (*func)(void*,char*,int);
	void *arg;
	const char *fmt;
	va_list ap;
{
	int c;                     /* Next character in the format string */
	char *bufpt;               /* Pointer to the conversion buffer */
	int precision;             /* Precision of the current field */
	int length;                /* Length of the field */
	int idx;                   /* A general purpose loop counter */
	int count;                 /* Total number of characters output */
	int width;                 /* Width of the current field */
	u_int8_t flag_leftjustify;   /* True if "-" flag is present */
	u_int8_t flag_plussign;      /* True if "+" flag is present */
	u_int8_t flag_blanksign;     /* True if " " flag is present */
	u_int8_t flag_alternateform; /* True if "#" flag is present */
	u_int8_t flag_zeropad;       /* True if field width constant starts
				      with zero */
	u_int8_t flag_long;          /* True if "l" flag is present */
	unsigned long longvalue;   /* Value for integer types */
	long_double_t realvalue; /* Value for real types */
	et_info_t *infop;            /* Pointer to the appropriate info
					structure */
	char buf[etBUFSIZE];       /* Conversion buffer */
	char prefix;               /* Prefix character.  "+" or "-" or " " or
				      '\0'. */
	u_int8_t errorflag = 0;      /* True if an error is encountered */
	u_int8_t xtype;              /* Conversion paradigm */
	char *extra;               /* Extra memory used for etTCLESCAPE
				      conversions */
	static char spaces[] =
      "                                                  "
      "                                                                      ";
#define etSPACESIZE (sizeof(spaces)-1)
#ifndef etNOFLOATINGPOINT
	int  exp;                  /* exponent of real numbers */
	double rounder;            /* Used for rounding floating point values*/
	u_int8_t flag_dp;            /* True if decimal point should be shown */
	u_int8_t flag_rtz;           /* True if trailing zeros should be
				      removed */
	u_int8_t flag_exp;           /* True to force display of the exponent */
	int nsd;                   /* Number of significant digits returned */
#endif

	count = length = 0;
	bufpt = 0;
	for(; (c = (*fmt)) != 0; ++fmt) {
		if (c != '%') {
			int amt;
			bufpt = (char *)fmt;
			amt = 1;
			while((c = (*++fmt)) != '%' && c != 0)
				amt++;
			(*func)(arg, bufpt, amt);
			count += amt;
			if (c == 0)
				break;
		}
		if ((c = (*++fmt)) == 0) {
			errorflag = 1;
			(*func)(arg, "%", 1);
			count++;
			break;
		}
		/* Find out what flags are present. */
		flag_leftjustify = flag_plussign = flag_blanksign = 
			flag_alternateform = flag_zeropad = 0;
		do {
			switch(c) {
			case '-':   flag_leftjustify = 1;     c = 0;   break;
			case '+':   flag_plussign = 1;        c = 0;   break;
			case ' ':   flag_blanksign = 1;       c = 0;   break;
			case '#':   flag_alternateform = 1;   c = 0;   break;
			case '0':   flag_zeropad = 1;         c = 0;   break;
			default:                                       break;
			}
		} while(c == 0 && (c = (*++fmt)) != 0);
		/* Get the field width. */
		width = 0;
		if (c == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flag_leftjustify = 1;
				width = -width;
			}
			c = *++fmt;
		} else {
			while(c >= '0' && c <= '9') {
				width = (width * 10) + c - '0';
				c = *++fmt;
			}
		}
		if (width > etBUFSIZE - 10) {
			width = etBUFSIZE - 10;
		}
		/* Get the precision. */
		if (c == '.') {
			precision = 0;
			c = *++fmt;
			if (c == '*') {
				precision = va_arg(ap,int);
#ifndef etCOMPATIBILITY
				/* This is sensible, but SUN OS 4.1 doesn't
				   do it. */
				if (precision < 0)
					precision = -precision;
#endif
				c = *++fmt;
			} else {
				while(c >= '0' && c <= '9') {
					precision = (precision * 10) + c - '0';
					c = *++fmt;
				}
			}
			/*
			 * Limit the precision to prevent overflowing buf[]
			 * during conversion
			 */
			if (precision > etBUFSIZE - 40)
				precision = etBUFSIZE - 40;
		} else {
			precision = -1;
		}
		/* Get the conversion type modifier. */
		if (c == 'l') {
			flag_long = 1;
			c = *++fmt;
		} else {
			flag_long = 0;
		}
		/* Fetch the info entry for the field. */
		infop = 0;
		xtype = etERROR;
		for(idx = 0; idx < etNINFO; idx++) {
			if (c == fmtinfo[idx].fmttype) {
				infop = &fmtinfo[idx];
				xtype = infop->type;
				break;
			}
		}
		extra = 0;

		/*
		 * At this point, variables are initialized as follows:
		 *
		 *   flag_alternateform          TRUE if a '#' is present.
		 *   flag_plussign               TRUE if a '+' is present.
		 *   flag_leftjustify            TRUE if a '-' is present or
		 *                               if the field width was
		 *                               negative.
		 *   flag_zeropad                TRUE if the width began
		 *                               with 0.
		 *   flag_long                   TRUE if the letter 'l' (ell)
		 *                               prefixed the conversion
		 *                               character.
		 *   flag_blanksign              TRUE if a ' ' is present.
		 *   width                       The specified field width.
		 *                               This is always non-negative.
		 *                               Zero is the default.
		 *   precision                   The specified precision.  The
		 *                               default is -1.
		 *   xtype                       The class of the conversion.
		 *   infop                       Pointer to the appropriate
		 *                               info struct.
		 */
		switch(xtype) {
		case etRADIX:
			if (flag_long) {
				longvalue = va_arg(ap, long);
			} else{
				longvalue = va_arg(ap, int);
			}
#ifdef etCOMPATIBILITY
			/*
			 * For the format %#x, the value zero is printed "0"
			 * not "0x0".  I think this is stupid.
			 */
			if (longvalue == 0)
				flag_alternateform = 0;
#else
			/*
			 * More sensible: turn off the prefix for octal
			 * (to prevent "00"), but leave the prefix for hex.
			 */
			if (longvalue == 0 && infop->base == 8)
				flag_alternateform = 0;
#endif
			if (infop->flags & FLAG_SIGNED) {
				if (*(long *)&longvalue < 0) {
					longvalue = -*(long *)&longvalue;
					prefix = '-';
				} else if (flag_plussign) {
					prefix = '+';
				} else if (flag_blanksign) {
					prefix = ' ';
				} else {
					prefix = 0;
				}
			} else {
				prefix = 0;
			}
			if (flag_zeropad && precision < width - (prefix != 0)){
				precision = width - (prefix != 0);
			}
			bufpt = &buf[etBUFSIZE];
			{
				register char *cset;
				register int base;
				cset = infop->charset;
				base = infop->base;
				do {                   /* Convert to ascii */
					*(--bufpt) = cset[longvalue % base];
					longvalue = longvalue / base;
				} while(longvalue > 0);
			}
			length = &buf[etBUFSIZE] - bufpt;
			for(idx = precision - length; idx > 0; idx--) {
				*(--bufpt) = '0';      /* Zero pad */
			}
			if (prefix)
				*(--bufpt) = prefix;   /* Add sign */
			if (flag_alternateform && infop->prefix) {
                                /* Add "0" or "0x" */
				char *pre, x;
				pre = infop->prefix;
				if (*bufpt != pre[0]) {
					for(pre = infop->prefix;
					    (x = (*pre)) != 0; pre++) {
						*(--bufpt) = x;
					}
				}
			}
			length = &buf[etBUFSIZE] - bufpt;
			break;
		case etFLOAT:
		case etEXP:
		case etGENERIC:
			realvalue = va_arg(ap, double);
#ifndef etNOFLOATINGPOINT
			if (precision < 0)
				precision = 6;      /* Set default precision */
			if (precision > etBUFSIZE - 10)
				precision = etBUFSIZE - 10;
			if (realvalue < 0.0) {
				realvalue = -realvalue;
				prefix = '-';
			} else {
				if (flag_plussign)
					prefix = '+';
				else if (flag_blanksign)
					prefix = ' ';
				else
					prefix = 0;
			}
			if (infop->type == etGENERIC && precision > 0)
				precision--;
			rounder = 0.0;
#ifdef COMPATIBILITY
			/*
			 * Rounding works like BSD when the constant 0.4999 is
			 * used.  Wierd!
			 */
			for(idx = precision, rounder = 0.4999; idx > 0;
			    idx--, rounder *= 0.1);
#else
			/*
			 * It makes more sense to use 0.5
			 */
			for(idx = precision, rounder = 0.5; idx > 0;
			    idx--, rounder *= 0.1);
#endif
			if (infop->type == etFLOAT)
				realvalue += rounder;
			/*
			 * Normalize realvalue to within 
			 * 10.0 > realvalue >= 1.0
			 */
			exp = 0;
			if (realvalue > 0.0) {
				int k = 0;
				while(realvalue >= 1e8 && k++ < 100) {
					realvalue *= 1e-8;
					exp+=8;
				}
				while(realvalue >= 10.0 && k++ < 100) {
					realvalue *= 0.1;
					exp++;
				}
				while(realvalue < 1e-8 && k++ < 100) {
					realvalue *= 1e8;
					exp-=8;
				}
				while(realvalue < 1.0 && k++ < 100) {
					realvalue *= 10.0;
					exp--;
				}
				if (k >= 100) {
					bufpt = "NaN";
					length = 3;
					break;
				}
			}
			bufpt = buf;
			/*
			 * If the field type is etGENERIC, then convert to
			 * either etEXP or etFLOAT, as appropriate.
			 */
			flag_exp = (xtype == etEXP);
			if (xtype != etFLOAT) {
				realvalue += rounder;
				if (realvalue >= 10.0) {
					realvalue *= 0.1;
					exp++;
				}
			}
			if (xtype == etGENERIC) {
				flag_rtz = !flag_alternateform;
				if (exp<-4 || exp > precision) {
					xtype = etEXP;
				} else {
					precision = precision - exp;
					xtype = etFLOAT;
				}
			} else {
				flag_rtz = 0;
			}
			/*
			 * The "exp+precision" test causes output to be of
			 * type etEXP if the precision is too large to fit
			 * in buf[].
			 */
			nsd = 0;
			if (xtype == etFLOAT &&
			    exp + precision < etBUFSIZE - 30) {
				flag_dp =(precision > 0 || flag_alternateform);
				if (prefix) {
					*(bufpt++) = prefix; /* Sign */
				}
				if (exp < 0) {
					*(bufpt++) = '0';    /* Digits before
								"." */
				} else {
					for(; exp >= 0; exp--) {
						*(bufpt++) =
							et_getdigit(&realvalue,
								    &nsd);
					}
				}
				if (flag_dp) {
					*(bufpt++) = '.';/* The decimal point*/
				}
				for(exp++; exp < 0 && precision > 0;
				    precision--, exp++) {
					*(bufpt++) = '0';
				}
				while((precision--) > 0) {
					*(bufpt++) = et_getdigit(&realvalue,
								 &nsd);
				}
				*(bufpt--) = 0;         /* Null terminate */
				if (flag_rtz && flag_dp) {
                                        /* Remove trailing zeros and "." */
					while( bufpt >= buf && *bufpt == '0') {
						*(bufpt--) = 0;
					}
					if (bufpt >= buf && *bufpt == '.' ) {
						*(bufpt--) = 0;
					}
				}
				bufpt++;        /* point to next free slot */
			} else {    /* etEXP or etGENERIC */
				flag_dp = (precision>0 || flag_alternateform);
				if (prefix) {
					*(bufpt++) = prefix;      /* Sign */
				}
                                /* First digit */
				*(bufpt++) = et_getdigit(&realvalue,&nsd);
				if (flag_dp) {
					*(bufpt++) = '.'; /* Decimal point */
				}
				while((precision--) > 0) {
					*(bufpt++) = et_getdigit(&realvalue,
								 &nsd);
				}
				bufpt--;            /* point to last digit */
				if (flag_rtz && flag_dp) {
					/* Remove tail zeros */
					while(bufpt >= buf && *bufpt == '0') {
						*(bufpt--) = 0;
					}
					if (bufpt >= buf && *bufpt == '.' ) {
						*(bufpt--) = 0;
					}
				}
				bufpt++;        /* point to next free slot */
				if (exp || flag_exp) {
					*(bufpt++) = infop->charset[0];
					if (exp < 0) { /* sign of exp */
						*(bufpt++) = '-';
						exp = -exp;
					} else {
						*(bufpt++) = '+';
					}
					if (exp >= 100) {
						/* 100's digit */
						*(bufpt++) = (exp/100) + '0';
						exp %= 100;
					}
					/* 10's digit */
					*(bufpt++) = exp/10+'0';
					/* 1's digit */
					*(bufpt++) = exp%10+'0';
				}
			}
			/*
			 * The converted number is in buf[] and zero
			 * terminated.  Output it.  Note that the number is
			 * in the usual order, not reversed as with integer
			 * conversions.
			 */
			length = bufpt - buf;
			bufpt = buf;

			/*
			 * Special Case:  Add leading zeros if the
			 * flag_zeropad flag is set and we are not left
			 * justified.
			 */
			if (flag_zeropad && !flag_leftjustify &&
			    length < width) {
				int i;
				int nPad = width - length;
				for(i = width; i >= nPad; i--) {
					bufpt[i] = bufpt[i - nPad];
				}
				i = (prefix != 0);
				while(nPad--)
					bufpt[i++] = '0';
				length = width;
			}
#endif
			break;
		case etSIZE:
			*(va_arg(ap,int*)) = count;
			length = width = 0;
			break;
		case etPERCENT:
			buf[0] = '%';
			bufpt = buf;
			length = 1;
			break;
		case etCHARLIT: /* FALLTHROUGH */
		case etCHARX:
			c = buf[0] = (xtype == etCHARX ?
				      va_arg(ap, int) : *++fmt);
			if (precision >= 0) {
				for(idx = 1; idx < precision; idx++) {
					buf[idx] = c;
				}
				length = precision;
			} else {
				length = 1;
			}
			bufpt = buf;
			break;
		case etSTRING:
		case etDYNSTRING:
			bufpt = va_arg(ap, char*);
			if (bufpt == 0) {
				bufpt = "";
			} else if (xtype == etDYNSTRING) {
				extra = bufpt;
			}
			length = strlen(bufpt);
			if (precision >= 0 && precision < length ) {
				length = precision;
			}
			break;
		case etSQLESCAPE: /* FALLTHROUGH */
		case etSQLESCAPE2:
		{
			int i, j, n, c, isnull;
			char *arg = va_arg(ap,char*);
			isnull = (arg == 0);
			if( isnull ) arg = (xtype == etSQLESCAPE2 ?
					    "NULL" : "(NULL)");
			for(i = n = 0; (c = arg[i]) != 0; i++) {
				if (c == '\'')
					n++;
			}
			n += i + 1 + ((!isnull && xtype == etSQLESCAPE2) ?
				      2 : 0);
			if (n > etBUFSIZE) {
				if (__dbsql_calloc(dbp, 1, n, extra) == ENOMEM)
					return -1;
				bufpt = extra;
			} else {
				bufpt = buf;
			}
			j = 0;
			if (!isnull && xtype == etSQLESCAPE2)
				bufpt[j++] = '\'';
			for(i = 0; (c = arg[i]) != 0; i++) {
				bufpt[j++] = c;
				if (c == '\'')
					bufpt[j++] = c;
			}
			if (!isnull && xtype == etSQLESCAPE2)
				bufpt[j++] = '\'';
			bufpt[j] = 0;
			length = j;
			if (precision >= 0 && precision < length)
				length = precision;
		}
		break;
		case etERROR:
			buf[0] = '%';
			buf[1] = c;
			errorflag = 0;
			idx = 1 + (c != 0);
			(*func)(arg, "%", idx);
			count += idx;
			if (c == 0)
				fmt--;
			break;
		} /* End switch over the format type */
		/*
		 * The text of the conversion is pointed to by "bufpt" and is
		 * "length" characters long.  The field width is "width".  Do
		 * the output.
		 */
		if (!flag_leftjustify) {
			register int nspace;
			nspace = width - length;
			if (nspace > 0) {
				count += nspace;
				while(nspace >= etSPACESIZE) {
					(*func)(arg, spaces, etSPACESIZE);
					nspace -= etSPACESIZE;
				}
				if (nspace > 0)
					(*func)(arg, spaces, nspace);
			}
		}
		if (length > 0) {
			(*func)(arg, bufpt, length);
			count += length;
		}
		if (flag_leftjustify) {
			register int nspace;
			nspace = width - length;
			if (nspace > 0) {
				count += nspace;
				while(nspace >= etSPACESIZE) {
					(*func)(arg, spaces, etSPACESIZE);
					nspace -= etSPACESIZE;
				}
				if (nspace > 0)
					(*func)(arg, spaces, nspace);
			}
		}
		if (extra) {
			if (xtype == etDYNSTRING) {
				__dbsql_free(dbp, extra); /* TODO which free? */
			} else {
				__dbsql_free(dbp, extra);
			}
		}
	} /* End for loop over the format string */
	return errorflag ? -1 : count;
}


/*
 * __mout -- 
 *	This function implements the callback from vxprintf. 
 *	This routine add nNewChar characters of text in zNewText to
 *	the sgMprintf structure pointed to by "arg".
 *
 * STATIC: static void mout __P((void *, char *, int));
 */
static void
__mout(arg, zNewText, nNewChar)
	void *arg;
	char *zNewText;
	int nNewChar;
{
	xvprintf_t *pM = (xvprintf_t*)arg;
	if (pM->len + nNewChar + 1 > pM->amt) {
		pM->amt = pM->len + (nNewChar * 2) + 1;
		if (pM->text == pM->base) {
			__dbsql_calloc(NULL, 1, pM->amt, &pM->text);
			if (pM->text && pM->len)
				memcpy(pM->text, pM->base, pM->len);
		} else {
			
			if (__dbsql_realloc(NULL, pM->amt, &pM->text) == ENOMEM) {
				__dbsql_free(NULL, pM->text);
				pM->len = 0;
				pM->amt = 0;
				pM->text = 0;
			}
		}
	}
	if (pM->text) {
		memcpy(&pM->text[pM->len], zNewText, nNewChar);
		pM->len += nNewChar;
		pM->text[pM->len] = 0;
	}
}

/*
 * xvprintf --
 *
 * PUBLIC: char *xvprintf __P((DBSQL *, const char *, va_list));
 */
char *
xvprintf(dbp, fmt, ap)
	DBSQL *dbp;
	const char *fmt;
	va_list ap;
{
	xvprintf_t s;
	char *new;
	char buf[200];

	s.len = 0;
	s.text = buf;
	s.amt = sizeof(buf);
	s.base = buf;
	__et_printf(dbp, __mout, &s, fmt, ap);
	s.text[s.len] = 0;
	__dbsql_malloc(dbp, s.len + 1, &new);
	if (new)
		strcpy(new, s.text);
	if (s.text != s.base)
		__dbsql_free(dbp, s.text);
	return new;
}

#ifdef CONFIG_TEST
/*
 * xprintf --
 *
 * PUBLIC: char *xprintf __P((DBSQL *, const char *, ...));
 */
char *
#ifdef STDC_HEADERS
xprintf(DBSQL *dbp, const char *fmt, ...)
#else
xprintf(dbp, fmt, va_alist)
	DBSQL *dbp;
	const char *fmt;
	va_dcl
#endif
{
	char *result;
	va_list ap;
	va_start(ap, fmt);
	result = xvprintf(dbp, fmt, ap);
	va_end(ap);
	return result;
}
#endif
