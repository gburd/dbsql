/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * $Id: sql_fns.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains the C functions that implement various SQL
 * functions.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#endif

#include "dbsql_int.h"

/* __min_func --
 *	Implementation of the non-aggregate min() function.
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
 *	Implementation of the non-aggregate and max() function.
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
 * __length_func --
 *	Implementation of the length() function.
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
 *	Implementation of the abs() function
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
 * __round_func --
 *	Implementation of the round() function.
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
			int code = code[in[i] & 0x7f];
			if (code > 0) {
				result[j++] = code + '0';
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
 */
static void
__random_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	u_int32_t n;
	static struct drand48_data rand;
	static int first_time = 1;
	if (first_time) {
		first_time = 0;
		srand48_r(&rand);
	}
	rand32_r(&rand, &n);
	dbsql_set_result_int(context, n);
}

/*
 * __last_inserted_rowid_func --
 *	Implementation of the last_inserted_rowid() SQL function.  The return
 *	value is the same as the __api_last_inserted_rowid() API function.
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

static void
__sum_finalize(context)
	dbsql_func_t *context;
{
	sum_ctx_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	dbsql_set_result_double(context, p ? p->sum : 0.0);
}

static void
__avg_finalize(context)
	dbsql_func_t *context;
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
 */
static void
__std_dev_finalize(context)
	dbsql_func_t *context;
{
	double rN = dbsql_aggregate_count(context);
	std_dev_ctx_t *p = dbsql_aggregate_context(context, sizeof(*p));
	if (p && p->cnt > 1) {
		double rCnt = p->cnt;
		dbsql_set_result_double(context, 
		      sqrt((p->sum2 - p->sum * p->sum / rCnt) / (rCnt - 1.0)));
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
 */
static void
__count_finalize(context)
	dbsql_func_t *context;
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
			if (__dbsql_calloc(NULL, 1, len + 1, &p->z) == ENOMEM)
				return;
		}
		strcpy(p->z, argv[0]);
	}
}

/*
 * __max_step --
 *	Routine to implement max() aggregate function.
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
			if (__dbsql_calloc(NULL, 1, len + 1, &p->z) == ENOMEM)
				return;
		}
		strcpy(p->z, argv[0]);
	}
}

/*
 * __min_max_finalize --
 */
static void
__min_max_finalize(context)
	dbsql_func_t *context;
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
 * __register_builtin_funcs --
 *	This function registered all of the above C functions as SQL
 *	functions.  This should be the only routine in this file with
 *	external linkage.
 *
 * PUBLIC: void __register_builtin_funcs __P((DBSQL *));
 */
void
__register_builtin_funcs(dbp)
	DBSQL *dbp;
{
	static struct {
		char *name;
		int num_arg;
		int data_type;
		void (*func)(dbsql_func_t*,int,const char**);
	} funcs[] = {
		{ "min",       -1, DBSQL_ARGS,    __min_func    },
		{ "min",        0, 0,              0          },
		{ "max",       -1, DBSQL_ARGS,    __max_func    },
		{ "max",        0, 0,              0          },
		{ "length",     1, DBSQL_NUMERIC, __length_func },
		{ "substr",     3, DBSQL_TEXT,    __substr_func },
		{ "abs",        1, DBSQL_NUMERIC, __abs_func    },
		{ "round",      1, DBSQL_NUMERIC, __round_func  },
		{ "round",      2, DBSQL_NUMERIC, __round_func  },
		{ "upper",      1, DBSQL_TEXT,    __upper_func  },
		{ "lower",      1, DBSQL_TEXT,    __lower_func  },
		{ "coalesce",  -1, DBSQL_ARGS,    __ifnull_func },
		{ "coalesce",   0, 0,              0          },
		{ "coalesce",   1, 0,              0          },
		{ "ifnull",     2, DBSQL_ARGS,    __ifnull_func },
		{ "random",    -1, DBSQL_NUMERIC, __random_func },
		{ "like",       2, DBSQL_NUMERIC, __like_func   },
		{ "glob",       2, DBSQL_NUMERIC, __glob_func   },
		{ "nullif",     2, DBSQL_ARGS,    __nullif_func },
		{ "dbsql_get_version",0,DBSQL_TEXT,  __version_func},
		{ "quote",      1, DBSQL_ARGS,    __quote_func  },
#ifdef DBSQL_SOUNDEX
		{ "soundex",    1, DBSQL_TEXT,    __soundex_func},
#endif
	};
	static struct {
		char *name;
		int num_arg;
		int data_type;
		void (*step)(dbsql_func_t*,int,const char**);
		void (*finalize)(dbsql_func_t*);
	} aggs[] = {
	{ "min",    1, 0,              __min_step,      __min_max_finalize },
	{ "max",    1, 0,              __max_step,      __min_max_finalize },
	{ "sum",    1, DBSQL_NUMERIC, __sum_step,       __sum_finalize    },
	{ "avg",    1, DBSQL_NUMERIC, __sum_step,       __avg_finalize    },
	{ "count",  0, DBSQL_NUMERIC, __count_step,     __count_finalize  },
	{ "count",  1, DBSQL_NUMERIC, __count_step,     __count_finalize  },
	{ "stddev", 1, DBSQL_NUMERIC, __std_dev_step,   __std_dev_finalize },
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

	dbp->create_function(dbp, "last_inserted_rowid", 0, DBSQL_UTF8_ENCODED,
			     dbp, __last_inserted_rowid_func, NULL, NULL);
	dbp->func_return_type(dbp, "last_inserted_rowid", DBSQL_NUMERIC);

	for(i = 0; i < sizeof(aggs) / sizeof(aggs[0]); i++) {
		dbp->create_function(dbp, aggs[i].name, aggs[i].num_arg,
				     DBSQL_UTF8_ENCODED, NULL,
				     NULL, aggs[i].step, aggs[i].finalize);
		dbp->func_return_type(dbp, aggs[i].name, aggs[i].data_type);
	}

	__register_datetime_funcs(dbp);
}
