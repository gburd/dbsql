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
 * This file contains routines that handle date and time functions.
 */

/*
 * IMPLEMENTATION NOTES:
 *
 * All times and dates are managed as Julian Day numbers.  The
 * dates and times are stored as the number of days since noon
 * in Greenwich on November 24, 4714 B.C. according to the Gregorian
 * calendar system.
 *
 * 1970-01-01 00:00:00 is JD 2440587.5
 * 2000-01-01 00:00:00 is JD 2451544.5
 *
 * This implemention requires years to be expressed as a 4-digit number
 * which means that only dates between 0000-01-01 and 9999-12-31 can
 * be represented, even though julian day numbers allow a much wider
 * range of dates.
 *
 * The Gregorian calendar system is used for all dates and times,
 * even those that predate the Gregorian calendar.  Historians usually
 * use the Julian calendar for dates prior to 1582-10-15 and for some
 * dates afterwards, depending on locale.  Beware of this difference.
 *
 * The conversion algorithms are implemented based on descriptions
 * in the following text:
 *
 *      Jean Meeus
 *      Astronomical Algorithms, 2nd Edition, 1998
 *      ISBN 0-943396-61-1
 *      Willmann-Bell, Inc
 *      Richmond, Virginia (USA)
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#endif

#include "dbsql_int.h"


#ifndef DBSQL_OMIT_DATETIME_FUNCS

/*
 * A structure for holding a single date and time.
 */
typedef struct {
	double rJD;      /* The julian day number */
	int Y, M, D;     /* Year, month, and day */
	int h, m;        /* Hour and minutes */
	int tz;          /* Timezone offset in minutes */
	double s;        /* Seconds */
	char validYMD;   /* True if Y,M,D are valid */
	char validHMS;   /* True if h,m,s are valid */
	char validJD;    /* True if rJD is valid */
	char validTZ;    /* True if tz is valid */
} datetime_t;


/*
 * __get_ndigits_as_int --
 *	Convert N digits from 'date' into an integer.  Return
 *	-1 if 'date' does not begin with N digits.
 *
 * STATIC: static int __get_ndigits_as_int __P((const char *, int));
 */
static int
__get_ndigits_as_int(date, n)
	const char *date;
	int n;
{
	int val = 0;
	while (n--) {
		if (!isdigit(*date))
			return -1;
		val = (val * 10) + (*date - '0');
		date++;
	}
	return val;
}

/*
 * __convert_str_to_double --
 *	Read text from z[] and convert into a floating point number.
 *	Place the result in 'result'.  Return the number of digits converted.
 *
 * STATIC: int __convert_str_to_double __P((const char *, double *));
 */
static int
__convert_str_to_double(date, result)
	const char *date;
	double *result;
{
	double r = 0.0;
	double divide = 1.0;
	int neg_p = 0;
	int num_char = 0;
	if (*date == '+') {
		date++;
		num_char++;
	} else if (*date == '-') {
		date++;
		neg_p = 1;
		num_char++;
	}
	if (!isdigit(*date))
		return 0;
	while(isdigit(*date)) {
		r = (r * 10.0) + (*date - '0');
		num_char++;
		date++;
	}
	if (*date == '.' && isdigit(date[1])) {
		date++;
		num_char++;
		while(isdigit(*date)) {
			r = (r * 10.0) + (*date - '0');
			divide *= 10.0;
			num_char++;
			date++;
		}
		r /= divide;
	}
	if (*date != 0 && !isspace(*date))
		return 0;
	*result = neg_p ? -r : r;
	return num_char;
}

/*
 * __parser_tz --
 *	Parse a timezone extension on the end of a date-time.
 *	The extension is of the form:
 *
 *	       (+/-)HH:MM
 *
 *	If the parse is successful, write the number of minutes
 *	of change in *pnMin and return 0.  If a parser error occurs,
 *	return 0.
 *
 *	A missing specifier is not considered an error.
 *
 * STATIC: int __parse_tz __P((const char *, datetime_t *));
 */
static int
__parse_tz(date, dt)
	const char *date;
	datetime_t *dt;
{
	int sgn = 0;
	int hr, mn;
	while(isspace(*date)) {
		date++;
	}
	dt->tz = 0;
	if (*date == '-') {
		sgn = -1;
	} else if (*date == '+') {
		sgn = +1;
	} else {
		return *date != 0;
	}
	date++;
	hr = __get_ndigits_as_int(date, 2);
	if (hr < 0 || hr > 14)
		return 1;
	date += 2;
	if (date[0] != ':')
		return 1;
	date++;
	mn = __get_ndigits_as_int(date, 2);
	if (mn < 0 || mn > 59)
		return 1;
	date += 2;
	dt->tz = sgn * (mn + (hr * 60));
	while(isspace(*date)) {
		date++;
	}
	return (*date != 0);
}

/*
 * __parse_hh_mm_ss --
 *	Parse times of the form HH:MM or HH:MM:SS or HH:MM:SS.FFFF.
 *	The HH, MM, and SS must each be exactly 2 digits.  The
 *	fractional seconds FFFF can be one or more digits.
 *
 *	Return 1 if there is a parsing error and 0 on success.
 *
 * STATIC: static int __parse_hh_mm_ss __P((const char *, datetime_t *));
 */
static int
__parse_hh_mm_ss(date, dt)
	const char *date;
	datetime_t *dt;
{
	int h, m, s;
	double ms = 0.0;
	h = __get_ndigits_as_int(date, 2);
	if (h < 0 || date[2] != ':')
		return 1;
	date += 3;
	m = __get_ndigits_as_int(date, 2);
	if (m < 0 || m > 59)
		return 1;
	date += 2;
	if (*date == ':') {
		s = __get_ndigits_as_int(&date[1], 2);
		if (s < 0 || s > 59)
			return 1;
		date += 3;
		if (*date == '.' && isdigit(date[1])) {
			double scale = 1.0;
			date++;
			while(isdigit(*date)) {
				ms = (ms * 10.0) + (*date - '0');
				scale *= 10.0;
				date++;
			}
			ms /= scale;
		}
	} else {
		s = 0;
	}
	dt->validJD = 0;
	dt->validHMS = 1;
	dt->h = h;
	dt->m = m;
	dt->s = s + ms;
	if (__parse_tz(date, dt))
		return 1;
	dt->validTZ = (dt->tz != 0);
	return 0;
}

/*
 * __compute_jd
 *	Convert from YYYY-MM-DD HH:MM:SS to julian day.  We always assume
 *	that the YYYY-MM-DD is according to the Gregorian calendar.
 *	Reference:  Meeus page 61
 *
 * STATIC: static void compute_jd __P((datetime_t *));
 */
static void
__compute_jd(dt)
	datetime_t *dt;
{
	int Y, M, D, A, B, X1, X2;

	if (dt->validJD)
		return;
	if(dt->validYMD) {
		Y = dt->Y;
		M = dt->M;
		D = dt->D;
	} else {
		Y = 2000;  /* If no YMD specified, assume 2000-Jan-01 */
		M = 1;
		D = 1;
	}
	if (M <= 2) {
		Y--;
		M += 12;
	}
	A = Y/100;
	B = 2 - A + (A/4);
	X1 = 365.25 * (Y + 4716);
	X2 = 30.6001 * (M + 1);
	dt->rJD = X1 + X2 + D + B - 1524.5;
	dt->validJD = 1;
	dt->validYMD = 0;
	if (dt->validHMS) {
		dt->rJD += ((dt->h * 3600.0) + (dt->m * 60.0) + dt->s)/86400.0;
		if (dt->validTZ) {
			dt->rJD += (dt->tz * 60) / 86400.0;
			dt->validHMS = 0;
			dt->validTZ = 0;
		}
	}
}

/*
 * __parse_yyyy_mm_dd --
 *	Parse dates of the form
 *
 *	    YYYY-MM-DD HH:MM:SS.FFF
 *	    YYYY-MM-DD HH:MM:SS
 *	    YYYY-MM-DD HH:MM
 *	    YYYY-MM-DD
 *
 *	Write the result into the datetime_t structure and return 0
 *	on success and 1 if the input string is not a well-formed
 *	date.
 *
 * STATIC: int __parse_yyyy_mm_dd __P((const char *, datetime_t *));
 */
static int
__parse_yyyy_mm_dd(date, dt)
	const char *date;
	datetime_t *dt;
{
	int Y, M, D;

	Y = __get_ndigits_as_int(date, 4);
	if (Y < 0 || date[4] != '-')
		return 1;
	date += 5;
	M = __get_ndigits_as_int(date, 2);
	if (M <= 0 || M > 12 || date[2] != '-')
		return 1;
	date += 3;
	D = __get_ndigits_as_int(date, 2);
	if (D <= 0 || D > 31)
		return 1;
	date += 2;
	while(isspace(*date)) {
		date++;
	}
	if (isdigit(*date)) {
		if (__parse_hh_mm_ss(date, dt))
			return 1;
	} else if (*date == 0) {
		dt->validHMS = 0;
	} else {
		return 1;
	}
	dt->validJD = 0;
	dt->validYMD = 1;
	dt->Y = Y;
	dt->M = M;
	dt->D = D;
	if (dt->validTZ) {
		__compute_jd(dt);
	}
	return 0;
}

/*
 * __parse_date_or_time --
 *	Attempt to parse the given string into a Julian Day Number.  Return
 *	the number of errors.
 *
 *	The following are acceptable forms for the input string:
 *
 *	     YYYY-MM-DD HH:MM:SS.FFF  +/-HH:MM
 *	     DDDD.DD 
 *	     now
 *
 *	In the first form, the +/-HH:MM is always optional.  The fractional
 *	seconds extension (the ".FFF") is optional.  The seconds portion
 *	(":SS.FFF") is option.  The year and date can be omitted as long
 *	as there is a time string.  The time string can be omitted as long
 *	as there is a year and date.
 *
 * STATIC: static int __parse_date_or_time __P((const char *, datetime_t *));
 */
static int
__parse_date_or_time(date, dt)
	const char *date;
	datetime_t *dt;
{
	int i = 0;
	memset(dt, 0, sizeof(*dt));
	while (isdigit(date[i])) {
		i++;
	}
	if (i == 4 && date[i] == '-') {
		return __parse_yyyy_mm_dd(date, dt);
	} else if (i == 2 && date[i] == ':') {
		return __parse_hh_mm_ss(date, dt);
		return 0;
	} else if (i == 0 && strcasecmp(date, "now") == 0) {
		double r;
		if (__os_jtime(&r) == 0) {
			dt->rJD = r;
			dt->validJD = 1;
			return 0;
		}
		return 1;
	} else if (__str_is_numeric(date)) {
		dt->rJD = __dbsql_atof(date);
		dt->validJD = 1;
		return 0;
	}
	return 1;
}

/*
 * __compute_ymd --
 *	Compute the Year, Month, and Day from the julian day number.
 *
 * STATIC: static void __compute_ymd __P((datetime_t *));
 */
static void
__compute_ymd(dt)
	datetime_t *dt;
{
	int Z, A, B, C, D, E, X1;
	if (dt->validYMD)
		return;
	Z = dt->rJD + 0.5;
	A = (Z - 1867216.25) / 36524.25;
	A = Z + 1 + A - (A/4);
	B = A + 1524;
	C = (B - 122.1)/365.25;
	D = 365.25 * C;
	E = (B - D) / 30.6001;
	X1 = 30.6001 * E;
	dt->D = B - D - X1;
	dt->M = (E < 14) ? (E - 1) : (E - 13);
	dt->Y = (dt->M > 2) ? (C - 4716) : (C - 4715);
	dt->validYMD = 1;
}

/*
 * __compute_hms --
 *	Compute the Hour, Minute, and Seconds from the julian day number.
 *
 * STATIC: static void __compute_hms __P((datetime_t *));
 */
static void
__compute_hms(dt)
	datetime_t *dt;
{
	int Z, s;
	if (dt->validHMS)
		return;
	Z = dt->rJD + 0.5;
	s = ((dt->rJD + 0.5 - Z) * 86400000.0) + 0.5;
	dt->s = 0.001 * s;
	s = dt->s;
	dt->s -= s;
	dt->h = s / 3600;
	s -= dt->h * 3600;
	dt->m = s / 60;
	dt->s += s - dt->m * 60;
	dt->validHMS = 1;
}

/*
 * __compute_ymd_hms --
 *	Compute both YMD and HMS
 *
 * STATIC: static void __compute_ymd_hms __P((datetime_t *));
 */
static void
__compute_ymd_hms(dt)
	datetime_t *dt;
{
	__compute_ymd(dt);
	__compute_hms(dt);
}

/*
 * __clear_ymd_hms_tz --
 *	Clear the YMD and HMS and the TZ.
 *
 * STATIC: static void __clear_ymd_hms_tz __P((datetime_t *));
 */
static void
__clear_ymd_hms_tz(dt)
	datetime_t *dt;
{
	dt->validYMD = 0;
	dt->validHMS = 0;
	dt->validTZ = 0;
}

/*
 * __localtime_offset --
 *	Compute the difference (in days) between localtime and UTC (a.k.a. GMT)
 *	for the time value 'dt' where 'dt' is in UTC.
 *
 * STATIC: static double __localtime_offset __P((datetime_t *));
 */
static double
__localtime_offset(dt)
	datetime_t *dt;
{
	datetime_t x, y;
	time_t t;
	struct tm tm;
	x = *dt;
	__compute_ymd_hms(&x);
	if (x.Y < 1971 || x.Y >= 2038) {
		x.Y = 2000;
		x.M = 1;
		x.D = 1;
		x.h = 0;
		x.m = 0;
		x.s = 0.0;
	} else {
		int s = x.s + 0.5;
		x.s = s;
	}
	x.tz = 0;
	x.validJD = 0;
	__compute_jd(&x);
	t = ((x.rJD - 2440587.5) * 86400.0) + 0.5;
	if (localtime_r(&t, &tm) == &tm) {
		y.Y = tm.tm_year + 1900;
		y.M = tm.tm_mon + 1;
		y.D = tm.tm_mday;
		y.h = tm.tm_hour;
		y.m = tm.tm_min;
		y.s = tm.tm_sec;
	}
	y.validYMD = 1;
	y.validHMS = 1;
	y.validJD = 0;
	y.validTZ = 0;
	__compute_jd(&y);
	return (y.rJD - x.rJD);
}

/*
 * __parse_modifier --
 *	Process a modifier to a date-time stamp.  The modifiers are
 *	as follows:
 *
 *	    NNN days
 *	    NNN hours
 *	    NNN minutes
 *	    NNN.NNNN seconds
 *	    NNN months
 *	    NNN years
 *	    start of month
 *	    start of year
 *	    start of week
 *	    start of day
 *	    weekday N
 *	    unixepoch
 *	    localtime
 *	    utc
 *
 *	Return 0 on success and 1 if there is any kind of error.
 *
 * STATIC: static int __parse_modifier __P((const char *, datetime_t *));
 */
static int
__parse_modifier(const char *mod, datetime_t *dt)
{
	int n, x, y;
	int rc = 1;
	double r, c1;
	char *z, buf[30];
	z = buf;
	for (n = 0; n < (sizeof(buf) - 1) && mod[n]; n++) {
		z[n] = tolower(mod[n]);
	}
	z[n] = 0;
	switch(z[0]) {
	case 'l':
		/*
		 * localtime
		 *
		 * Assuming the current time value is UTC (a.k.a. GMT),
		 * shift it to show local time.
		 */
		if (strcmp(z, "localtime") == 0) {
			__compute_jd(dt);
			dt->rJD += __localtime_offset(dt);
			__clear_ymd_hms_tz(dt);
			rc = 0;
		}
		break;
	case 'u':
		/*
		 *    unixepoch
		 *
		 * Treat the current value of dt->rJD as the number of
		 * seconds since 1970.  Convert to a real julian day number.
		 */
		if (strcmp(z, "unixepoch") == 0 && dt->validJD) {
			dt->rJD = (dt->rJD / 86400.0) + 2440587.5;
			__clear_ymd_hms_tz(dt);
			rc = 0;
		} else if (strcmp(z, "utc") == 0) {
			__compute_jd(dt);
			c1 = __localtime_offset(dt);
			dt->rJD -= c1;
			__clear_ymd_hms_tz(dt);
			dt->rJD += (c1 - __localtime_offset(dt));
			rc = 0;
		}
		break;
	case 'w':
		/*
		 *    weekday N
		 *
		 * Move the date to the same time on the next occurrance of
		 * weekday N where 0==Sunday, 1==Monday, and so forth.  If the
		 * date is already on the appropriate weekday, this is a no-op.
		 */
		if (strncmp(z, "weekday ", 8) == 0 &&
		    (__convert_str_to_double(&z[8], &r) > 0) &&
		    ((n = r) == r) && (n >= 0) && (r < 7)) {
			int Z;
			__compute_ymd_hms(dt);
			dt->validTZ = 0;
			dt->validJD = 0;
			__compute_jd(dt);
			Z = dt->rJD + 1.5;
			Z %= 7;
			if (Z > n)
				Z -= 7;
			dt->rJD += (n - Z);
			__clear_ymd_hms_tz(dt);
			rc = 0;
		}
		break;
	case 's':
		/*
		 *    start of TTTTT
		 *
		 * Move the date backwards to the beginning of the current day,
		 * or month or year.
		 */
		if (strncmp(z, "start of ", 9) != 0)
			break;
		z += 9;
		__compute_ymd(dt);
		dt->validHMS = 1;
		dt->h = dt->m = 0;
		dt->s = 0.0;
		dt->validTZ = 0;
		dt->validJD = 0;
		if (strcmp(z, "month") == 0) {
			dt->D = 1;
			rc = 0;
		} else if (strcmp(z, "year") == 0) {
			__compute_ymd(dt);
			dt->M = 1;
			dt->D = 1;
			rc = 0;
		} else if (strcmp(z, "day") == 0) {
			rc = 0;
		}
		break;
	case '+': /* FALLTHROUGH */
	case '-': /* FALLTHROUGH */
	case '0': /* FALLTHROUGH */
	case '1': /* FALLTHROUGH */
	case '2': /* FALLTHROUGH */
	case '3': /* FALLTHROUGH */
	case '4': /* FALLTHROUGH */
	case '5': /* FALLTHROUGH */
	case '6': /* FALLTHROUGH */
	case '7': /* FALLTHROUGH */
	case '8': /* FALLTHROUGH */
	case '9':
		n = __convert_str_to_double(z, &r);
		if (n <= 0)
			break;
		z += n;
		while(isspace(z[0]))
			z++;
		n = strlen(z);
		if (n > 10 || n < 3)
			break;
		if (z[n-1] == 's') {
			z[n-1] = 0;
			n--;
		}
		__compute_jd(dt);
		rc = 0;
		if (n == 3 && strcmp(z, "day") == 0) {
			dt->rJD += r;
		} else if (n == 4 && strcmp(z, "hour") == 0) {
			dt->rJD += (r / 24.0);
		} else if (n == 6 && strcmp(z, "minute") == 0) {
			dt->rJD += (r / (24.0 * 60.0));
		} else if (n == 6 && strcmp(z, "second") == 0) {
			dt->rJD += (r / (24.0 * 60.0 * 60.0));
		} else if (n == 5 && strcmp(z, "month") == 0) {
			__compute_ymd_hms(dt);
			dt->M += r;
			x = (dt->M > 0) ?
				((dt->M - 1) / 12) :
				((dt->M-12) / 12);
			dt->Y += x;
			dt->M -= (x * 12);
			dt->validJD = 0;
			__compute_jd(dt);
			y = r;
			if (y != r) {
				dt->rJD += ((r - y) * 30.0);
			}
		} else if (n == 4 && strcmp(z, "year") == 0) {
			__compute_ymd_hms(dt);
			dt->Y += r;
			dt->validJD = 0;
			__compute_jd(dt);
		} else {
			rc = 1;
		}
		__clear_ymd_hms_tz(dt);
		break;
	default:
		break;
	}
	return rc;
}

/*
 * __is_date --
 *	Process time function arguments.  argv[0] is a date-time stamp.
 *	argv[1] and following are modifiers.  Parse them all and write
 *	the resulting time into the datetime_t structure p.  Return 0
 *	on success and 1 if there are any errors.
 *
 * STATIC: static int __is_date __P((int, const char **, datetime_t *));
 */
static int
__is_date(argc, argv, dt)
	int argc;
	const char **argv;
	datetime_t *dt;
{
	int i;
	if (argc == 0)
		return 1;
	if (argv[0] == 0 || __parse_date_or_time(argv[0], dt))
		return 1;
	for (i = 1; i < argc; i++) {
		if (argv[i] == 0 || __parse_modifier(argv[i], dt))
			return 1;
	}
	return 0;
}

/*
 * __julianday_sql_func --
 *	   julianday( TIMESTRING, MOD, MOD, ...)
 *
 *	Return the julian day number of the date specified in the arguments
 *
 * STATIC: static void __julianday_sql_func __P((dbsql_func_t *, int,
 * STATIC:             const char **));
 */
static void
__julianday_sql_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	datetime_t dt;
	if (__is_date(argc, argv, &dt) == 0) {
		__compute_jd(&dt);
		dbsql_set_result_double(context, dt.rJD);
	}
}

/*
 * __datetime_sql_func --
 *	   datetime( TIMESTRING, MOD, MOD, ...)
 *
 *	Return YYYY-MM-DD HH:MM:SS
 *
 * STATIC: static void __datetime__sql_func __P((dbsql_func_t *, int,
 * STATIC:             const char **));
 */
static void
__datetime_sql_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	datetime_t dt;
	if (__is_date(argc, argv, &dt) == 0) {
		char buf[100];
		__compute_ymd_hms(&dt);
		sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
			dt.Y, dt.M, dt.D, dt.h, dt.m, (int)(dt.s));
		dbsql_set_result_string(context, buf, -1);
	}
}

/*
 * __time_sql_func --
 *	   time( TIMESTRING, MOD, MOD, ...)
 *
 *	Return HH:MM:SS
 * STATIC: static void __time_sql_func __P((dbsql_func_t *, int, const char **));
 */
static void
__time_sql_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	datetime_t dt;
	if (__is_date(argc, argv, &dt) == 0) {
		char buf[100];
		__compute_hms(&dt);
		sprintf(buf, "%02d:%02d:%02d", dt.h, dt.m, (int)dt.s);
		dbsql_set_result_string(context, buf, -1);
	}
}

/*
 * __date_sql_func --
 *	   date( TIMESTRING, MOD, MOD, ...)
 *
 *	Return YYYY-MM-DD
 *
 * STATIC: static void __date_sql_func __P((dbsql_func_t *, int, const char **));
 */
static void
__date_sql_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	datetime_t dt;
	if (__is_date(argc, argv, &dt) == 0) {
		char buf[100];
		__compute_ymd(&dt);
		sprintf(buf, "%04d-%02d-%02d", dt.Y, dt.M, dt.D);
		dbsql_set_result_string(context, buf, -1);
	}
}

/*
 * __strftime_sql_func --
 *	   strftime( FORMAT, TIMESTRING, MOD, MOD, ...)
 *
 *	Return a string described by FORMAT.  Conversions as follows:
 *
 *	   %d  day of month
 *	   %f  ** fractional seconds  SS.SSS
 *	   %H  hour 00-24
 *	   %j  day of year 000-366
 *	   %J  ** Julian day number
 *	   %m  month 01-12
 *	   %M  minute 00-59
 *	   %s  seconds since 1970-01-01
 *	   %S  seconds 00-59
 *	   %w  day of week 0-6  sunday==0
 *	   %W  week of year 00-53
 *	   %Y  year 0000-9999
 *	   %%  %
 *
 * STATIC: static void __strftime_sql_func __P((dbsql_func_t *, int,
 * STATIC:             const char **));
*/
static void
__strftime_sql_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	datetime_t dt;
	int n, i, j;
	char *z;
	const char *fmt = argv[0];
	char buf[100];

	if (argv[0] == 0 || __is_date((argc - 1), (argv + 1), &dt))
		return;

	for (i = 0, n = 1; fmt[i]; i++, n++) {
		if (fmt[i] == '%') {
			switch(fmt[i + 1]) {
			case 'd': /* FALLTHROUGH */
			case 'H': /* FALLTHROUGH */
			case 'm': /* FALLTHROUGH */
			case 'M': /* FALLTHROUGH */
			case 'S': /* FALLTHROUGH */
			case 'W':
				n++;
				/* FALLTHROUGH */
			case 'w':
			case '%':
				break;
			case 'f':
				n += 8;
				break;
			case 'j':
				n += 3;
				break;
			case 'Y':
				n += 8;
				break;
			case 's': /* FALLTHROUGH */
			case 'J':
				n += 50;
				break;
			default:
				return;  /* ERROR, return a NULL. */
			}
			i++;
		}
	}
	if (n < sizeof(buf)) {
		z = buf;
	} else {
		if (__dbsql_malloc(NULL, n, &z) == ENOMEM)
			return;
	}
	__compute_jd(&dt);
	__compute_ymd_hms(&dt);
	for (i = j = 0; fmt[i]; i++) {
		if (fmt[i] != '%') {
			z[j++] = fmt[i];
		} else {
			i++;
			switch(fmt[i]) {
			case 'd':
				sprintf(&z[j],"%02d",dt.D);
				j += 2;
				break;
			case 'f': {
				int s = dt.s;
				int ms = (dt.s - s) * 1000.0;
				sprintf(&z[j], "%02d.%03d", s, ms);
				j += strlen(&z[j]);
				break;
			}
			case 'H':
				sprintf(&z[j], "%02d", dt.h);
				j += 2;
				break;
			case 'W': /* FALLTHROUGH */
			case 'j': {
				int n;
				datetime_t y = dt;
				y.validJD = 0;
				y.M = 1;
				y.D = 1;
				__compute_jd(&y);
				n = dt.rJD - y.rJD + 1;
				if (fmt[i] == 'W') {
					sprintf(&z[j], "%02d", (n + 6) / 7);
					j += 2;
				} else {
					sprintf(&z[j], "%03d", n);
					j += 3;
				}
				break;
			}
			case 'J':
				sprintf(&z[j], "%.16g", dt.rJD);
				j += strlen(&z[j]);
				break;
			case 'm':
				sprintf(&z[j], "%02d", dt.M);
				j += 2;
				break;
			case 'M':
				sprintf(&z[j], "%02d", dt.m);
				j += 2;
				break;
			case 's':
				sprintf(&z[j], "%d",
				      (int)((dt.rJD-2440587.5)*86400.0 + 0.5));
				j += strlen(&z[j]);
				break;
			case 'S':
				sprintf(&z[j], "%02d", (int)(dt.s + 0.5));
				j += 2;
				break;
			case 'w':
				z[j++] = (((int)(dt.rJD + 1.5)) % 7) + '0';
				break;
			case 'Y':
				sprintf(&z[j], "%04d", dt.Y);
				j += strlen(&z[j]);
				break;
			case '%':
				z[j++] = '%';
				break;
			}
		}
	}
	z[j] = 0;
	dbsql_set_result_string(context, z, -1);
	if (z != buf) {
		__dbsql_free(NULL, z);
	}
}


#endif

/*
 * __register_datetime_funcs --
 *	This function registered all of the above C functions as SQL
 *	functions.  This should be the only routine in this file with
 *	external linkage.
 *
 * PUBLIC: void __register_datetime_funcs __P((DBSQL *));
 */
void
__register_datetime_funcs(dbp)
	DBSQL *dbp;
{
	static struct {
		char *name;
		int args;
		int type;
		void (*func)(dbsql_func_t*, int, const char**);
	} funcs[] = {
#ifndef DBSQL_OMIT_DATETIME_FUNCS
		{ "julianday", -1, DBSQL_NUMERIC, __julianday_sql_func   },
		{ "date",      -1, DBSQL_TEXT,    __date_sql_func        },
		{ "time",      -1, DBSQL_TEXT,    __time_sql_func        },
		{ "datetime",  -1, DBSQL_TEXT,    __datetime_sql_func    },
		{ "strftime",  -1, DBSQL_TEXT,    __strftime_sql_func    },
#endif
	};
	int i;

	for (i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
		dbp->create_function(dbp, funcs[i].name, funcs[i].args,
				     DBSQL_UTF8_ENCODED, NULL, funcs[i].func,
				     NULL, NULL);
		if (funcs[i].func) {
			dbp->func_return_type(dbp, funcs[i].name,
					      funcs[i].type);
		}
	}
}
