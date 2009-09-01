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

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>				/* Declare STDERR_FILENO. */
#endif

#include "dbsql_int.h"

#ifdef DIAGNOSTIC
/*
 * __dbsql_assert --
 *	Error when an assertion fails.  Only checked if #DIAGNOSTIC defined.
 *
 * PUBLIC: #ifdef DIAGNOSTIC
 * PUBLIC: void __dbsql_assert __P((const char *, const char *, int));
 * PUBLIC: #endif
 */
void
__dbsql_assert(failedexpr, file, line)
	const char *failedexpr, *file;
	int line;
{
	(void)fprintf(stderr,
	    "__dbsql_assert: \"%s\" failed: file \"%s\", line %d\n",
	    failedexpr, file, line);
	(void)fflush(stderr);

	/* We want a stack trace of how this could possibly happen. */
	abort();

	/* NOTREACHED */
}
#endif

/*
 * __dbsql_panic_msg --
 *	Just report that someone else paniced.
 *
 * PUBLIC: int __dbsql_panic_msg __P((DBSQL *));
 */
int
__dbsql_panic_msg(dbp)
	DBSQL *dbp;
{
	__dbsql_err(dbp, "PANIC: fatal database error detected; run recovery");

	if (dbp && dbp->dbsql_paniccall != NULL)
		dbp->dbsql_paniccall(dbp, DB_RUNRECOVERY);

	return (DB_RUNRECOVERY);
}

/*
 * __dbsql_panic --
 *	Lock out the database due to unrecoverable error.
 *
 * PUBLIC: int __dbsql_panic __P((DBSQL *, int));
 */
int
__dbsql_panic(dbp, errval)
	DBSQL *dbp;
	int errval;
{
	if (dbp != NULL) {
		PANIC_SET(dbp, 1);

		__dbsql_err(dbp, "PANIC: %s", dbsql_strerror(errval));

		if (dbp->dbsql_paniccall != NULL)
			dbp->dbsql_paniccall(dbp, errval);
	}

#if defined(DIAGNOSTIC) && !defined(CONFIG_TEST)
	/*
	 * We want a stack trace of how this could possibly happen.
	 *
	 * Don't drop core if it's the test suite -- it's reasonable for the
	 * test suite to check to make sure that DBSQL_RUNRECOVERY is returned
	 * under certain conditions.
	 */
	abort();
#endif

	/*
	 * Chaos reigns within.
	 * Reflect, repent, and reboot.
	 * Order shall return.
	 */
	return (DBSQL_RUNRECOVERY);
}

/*
 * dbsql_strerror --
 *	ANSI C strerror(3) for DBSQL.
 *
 * EXTERN: char *dbsql_strerror __P((int));
 */
char *
dbsql_strerror(error)
	int error;
{
	char *p;

	if (error == 0)
		return ("Successful return: 0");
	if (error > 0) {
		if ((p = strerror(error)) != NULL)
			return (p);
		goto unknown_err;
	}
	if (error <= -30999 && error >= -30800) {
		if ((p = db_strerror(error)) != NULL)
			return (p);
		goto unknown_err;
	}

	/*
	 * !!!
	 * The Tcl API requires that some of these return strings be compared
	 * against strings stored in application scripts.  So, any of these
	 * errors that do not invariably result in a Tcl exception may not be
	 * altered.
	 */
	switch (error) {
	case DBSQL_ERROR:
		return ("DBSQL_ERROR: SQL logic error or missing database");
	case DBSQL_INTERNAL:
		return ("DBSQL_INTERNAL: Internal implementation flaw");
	case DBSQL_PERM:
		return ("DBSQL_PERM: Access denied due to permissions.");
	case DBSQL_ABORT:
		return ("DBSQL_ABORT: Callback requested query abort");
	case DBSQL_BUSY:
		return ("DBSQL_BUSY: Database is locked");
	case DBSQL_LOCKED:
		return ("DBSQL_LOCKED: Database table is locked");
	case DBSQL_NOMEM:
		return ("DBSQL_NOMEM: Unable to allocate additional memory.");
	case DBSQL_READONLY:
		return
		      ("DBSQL_READONLY: Attempt to write a readonly database");
	case DBSQL_INTERRUPTED:
		return ("DBSQL_INTERRUPTED: Interrupted during processing");
	case DBSQL_IOERR:
		return ("DBSQL_IOERROR: Disk I/O error");
	case DBSQL_NOTFOUND:
		return ("DBSQL_NOTFOUND: Table or record not found");
	case DBSQL_FULL:
		return ("DBSQL_FULL: Database is full");
	case DBSQL_CANTOPEN:
		return ("DBSQL_CANTOPEN: Unable to open database file");
	case DBSQL_PROTOCOL:
		return ("DBSQL_PROTOCOL: Database locking protocol failure");
	case DBSQL_EMPTY:
		return ("DBSQL_EMPTY: Table contains no data");
	case DBSQL_SCHEMA:
		return ("DBSQL_SCHEMA: Database schema has changed");
	case DBSQL_CONSTRAINT:
		return ("DBSQL_CONSTRAINT: Constraint failed");
	case DBSQL_MISMATCH:
		return ("DBSQL_MISMATCH: Datatype mismatch");
	case DBSQL_MISUSE:
		return
		      ("DBSQL_MISUSE: Library routine called out of sequence");
	case DBSQL_AUTH:
		return ("DBSQL_AUTH: Authorization denied");
	case DBSQL_FORMAT:
		return ("DBSQL_FORMAT: Auxiliary database format error");
	case DBSQL_RANGE:
		return ("DBSQL_RANGE: Bind index out of range");
	case DBSQL_CORRUPT:
		return ("DBSQL_CORRUPT: Data record is malformed");
	case DBSQL_RUNRECOVERY:
		return (
  "DBSQL_RUNRECOVERY: Shutdown and run recovery on the database environment.");
	case DBSQL_INVALID_NAME:
		return (
		"DBSQL_INVALID_NAME: Empty or invalid file name supplied");
	}

unknown_err: {
		/*
		 * !!!
		 * Room for a 64-bit number + slop.  This buffer is only used
		 * if we're given an unknown error, which should never happen.
		 * Note, however, we're no longer thread-safe if it does.
		 */
		static char ebuf[40];

		(void)snprintf(ebuf, sizeof(ebuf), "Unknown error: %d", error);
		return (ebuf);
	}
}

/*
 * __dbsql_err --
 *	Standard DBSQL error routine.  The same as errx, except we don't write
 *	to stderr if no output mechanism was specified.
 *
 * PUBLIC: void __dbsql_err __P((const DBSQL *, const char *, ...))
 * PUBLIC:      __attribute__ ((__format__ (__printf__, 2, 3)));
 */
void
#ifdef STDC_HEADERS
__dbsql_err(const DBSQL *dbp, const char *fmt, ...)
#else
__dbsql_err(dbp, fmt, va_alist)
	const DBSQL *dbp;
	const char *fmt;
	va_dcl
#endif
{
	DBSQL_REAL_ERR(dbp, 0, 0, 0, fmt);
}

#ifndef HAVE_VSNPRINTF
#define	OVERFLOW_ERROR	"internal buffer overflow, process aborted\n"
#ifndef	STDERR_FILENO
#define	STDERR_FILENO	2
#endif
#endif

/*
 * __dbsql_errcall --
 *	Do the error message work for callback functions.
 *
 * PUBLIC: void __dbsql_errcall __P((const DBSQL *, int, int, const char *,
 * PUBLIC:                      va_list));
 */
void
__dbsql_errcall(dbp, error, error_set, fmt, ap)
	const DBSQL *dbp;
	int error, error_set;
	const char *fmt;
	va_list ap;
{
	char *p;
	char errbuf[2048]; /* !!!: END OF THE STACK DON'T TRUST SPRINTF. */

	p = errbuf;
	if (fmt != NULL)
		p += vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
	if (error_set)
		p += snprintf(p,
		    sizeof(errbuf) - (size_t)(p - errbuf), ": %s",
		    dbsql_strerror(error));
#ifndef HAVE_VSNPRINTF
	/*
	 * !!!
	 * We're potentially manipulating strings handed us by the application,
	 * and on systems without a real snprintf() the sprintf() calls could
	 * have overflowed the buffer.  We can't do anything about it now, but
	 * we don't want to return control to the application, we might have
	 * overwritten the stack with a Trojan horse.  We're not trying to do
	 * anything recoverable here because systems without snprintf support
	 * are pretty rare anymore.
	 */
	if ((size_t)(p - errbuf) > sizeof(errbuf)) {
		write(
		    STDERR_FILENO, OVERFLOW_ERROR, sizeof(OVERFLOW_ERROR) - 1);
		abort();
		/* NOTREACHED */
	}
#endif
	dbp->dbsql_errcall(dbp->dbsql_errpfx, errbuf);
}

/*
 * __dbsql_errfile --
 *	Do the error message work for FILE *s.
 *
 * PUBLIC: void __dbsql_errfile
 * PUBLIC:          __P((const DBSQL *, int, int, const char *, va_list));
 */
void
__dbsql_errfile(dbp, error, error_set, fmt, ap)
	const DBSQL *dbp;
	int error, error_set;
	const char *fmt;
	va_list ap;
{
	FILE *fp;

	fp = dbp == NULL ||
	    dbp->dbsql_errfile == NULL ? stderr : dbp->dbsql_errfile;

	if (dbp != NULL && dbp->dbsql_errpfx != NULL)
		(void)fprintf(fp, "%s: ", dbp->dbsql_errpfx);
	if (fmt != NULL) {
		(void)vfprintf(fp, fmt, ap);
		if (error_set)
			(void)fprintf(fp, ": ");
	}
	if (error_set)
		(void)fprintf(fp, "%s", dbsql_strerror(error));
	(void)fprintf(fp, "\n");
	(void)fflush(fp);
}

/*
 * __error_msg --
 *	Add an error message to pParse->zErrMsg and increment pParse->nErr.
 *	The following formatting characters are allowed:
 *
 *	      %s      Insert a string
 *	      %z      A string that should be freed after use
 *	      %d      Insert an integer
 *	      %T      Insert a token
 *	      %S      Insert the first element of a SrcList
 *
 * PUBLIC: void __error_msg __P((parser_t *, const char *, ...));
 */
void
#ifdef STDC_HEADERS
__error_msg(parser_t *parser, const char *fmt, ...)
#else
__error_msg(parser, fmt, va_alist)
	parser_t parser;
	const char *fmt;
	va_dcl
#endif
{
	va_list ap;
	int len;
	int i, j;
	char *z;
	static char null[] = "NULL";

	parser->nErr++;
	len = 1 + strlen(fmt);
	va_start(ap, fmt);
	for(i = 0; fmt[i]; i++) {
		if (fmt[i] != '%' || fmt[i + 1] == 0)
			continue;
		i++;
		switch(fmt[i]) {
		case 'd': {
			(void)va_arg(ap, int);
			len += 20;
			break;
		}
		case 'z': /* FALLTHROUGH */
		case 's': {
			char *z2 = va_arg(ap, char*);
			if (z2 == 0)
				z2 = null;
			len += strlen(z2);
			break;
		}
		case 'T': {
			token_t *p = va_arg(ap, token_t*);
			len += p->n;
			break;
		}
		case 'S': {
			src_list_t *p = va_arg(ap, src_list_t*);
			int k = va_arg(ap, int);
			DBSQL_ASSERT(p->nSrc > k && k >= 0);
			len += strlen(p->a[k].zName);
			if (p->a[k].zDatabase && p->a[k].zDatabase[0]) {
				len += strlen(p->a[k].zDatabase) + 1;
			}
			break;
		}
		default:
			len++;
			break;
		}
	}
	va_end(ap);
	__dbsql_calloc(parser->db, 1, len, &z);
	if (z == 0)
		return;
	__dbsql_free(parser->db, parser->zErrMsg);
	parser->zErrMsg = z;
	va_start(ap, fmt);
	for(i = j = 0; fmt[i]; i++) {
		if (fmt[i] != '%' || fmt[i + 1] == 0)
			continue;
		if (i > j) {
			memcpy(z, &fmt[j], i - j);
			z += i - j;
		}
		j = i + 2;
		i++;
		switch(fmt[i]) {
		case 'd': {
			int x = va_arg(ap, int);
			sprintf(z, "%d", x);
			z += strlen(z);
			break;
		}
		case 'z': /* FALLTHROUGH */
		case 's': {
			int len;
			char *z2 = va_arg(ap, char*);
			if (z2 == 0)
				z2 = null;
			len = strlen(z2);
			memcpy(z, z2, len);
			z += len;
			if (fmt[i] == 'z' && z2 != null) {
				__dbsql_free(NULL, z2);
			}
			break;
		}
		case 'T': {
			token_t *p = va_arg(ap, token_t*);
			memcpy(z, p->z, p->n);
			z += p->n;
			break;
		}
		case 'S': {
			int len;
			src_list_t *p = va_arg(ap, src_list_t*);
			int k = va_arg(ap, int);
			assert(p->nSrc > k && k >= 0);
			if (p->a[k].zDatabase && p->a[k].zDatabase[0]) {
				len = strlen(p->a[k].zDatabase);
				memcpy(z, p->a[k].zDatabase, len);
				z += len;
				*(z++) = '.';
			}
			len = strlen(p->a[k].zName);
			memcpy(z, p->a[k].zName, len);
			z += len;
			break;
		}
		default:
			*(z++) = fmt[i];
			break;
		}
	}
	va_end(ap);
	if (i > j) {
		memcpy(z, &fmt[j], i - j);
		z += i - j;
	}
	DBSQL_ASSERT((z - parser->zErrMsg) < len);
	*z = 0;
}
