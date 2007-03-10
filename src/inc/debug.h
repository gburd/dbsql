/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1998-2004
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: debug.h 7 2007-02-03 13:34:17Z gburd $
 */

#ifndef _DBSQL_DEBUG_H_
#define	_DBSQL_DEBUG_H_

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Turn on additional error checking in gcc 3.X.
 */
#if !defined(__GNUC__) || __GNUC__ < 2 || (__GNUC__ == 2 && __GNUC_MINOR__ < 5)
#define	__attribute__(s)
#endif

/*
 * When running with #DIAGNOSTIC defined, we smash memory and do memory
 * guarding with a special byte value.
 */
#define	CLEAR_BYTE	0xdb
#define	GUARD_BYTE	0xdc

/*
 * DBSQL assertions.
 *
 * Use __STDC__ rather than STDC_HEADERS, the #e construct is ANSI C specific.
 */
#if defined(__STDC__) && defined(DIAGNOSTIC)
#define	DBSQL_ASSERT(e)	((e) ? (void)0 :__dbsql_assert(#e, __FILE__, __LINE__))
#else
#define	DBSQL_ASSERT(e)
#endif

/*
 * "Shut that bloody compiler up!"
 *
 * Unused, or not-used-yet variable.  We need to write and then read the
 * variable, some compilers are too bloody clever by half.
 */
#define	COMPQUIET(n, v)							\
	(n) = (v);							\
	(n) = (n)

/*
 * Purify and other run-time tools complain about uninitialized reads/writes
 * of structure fields whose only purpose is padding, as well as when heap
 * memory that was never initialized is written to disk.
 */
#ifdef	UMRW
#define	UMRW_SET(v)	(v) = 0
#else
#define	UMRW_SET(v)
#endif

/*
 * Error message handling.  Use a macro instead of a function because va_list
 * references to variadic arguments cannot be reset to the beginning of the
 * variadic argument list (and then rescanned), by functions other than the
 * original routine that took the variadic list of arguments.
 */
#if defined(STDC_HEADERS) || defined(__cplusplus)
#define	DBSQL_REAL_ERR(dbp, error, error_set, stderr_default, fmt) {	\
	va_list ap;							\
									\
	/* Call the user's callback function, if specified. */		\
	va_start(ap, fmt);						\
	if ((dbp) != NULL && (dbp)->dbsql_errcall != NULL)		\
		__dbsql_errcall(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
									\
	/* Write to the user's file descriptor, if specified. */	\
	va_start(ap, fmt);						\
	if ((dbp) != NULL && (dbp)->dbsql_errfile != NULL)		\
		__dbsql_errfile(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
									\
	/*								\
	 * If we have a default and we didn't do either of the above,	\
	 * write to the default.					\
	 */								\
	va_start(ap, fmt);						\
	if ((stderr_default) && ((dbp) == NULL ||			\
	    ((dbp)->dbsql_errcall == NULL && (dbp)->dbsql_errfile == NULL)))\
		__dbsql_errfile(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
}
#else
#define	DBSQL_REAL_ERR(dbp, error, error_set, stderr_default, fmt) {	\
	va_list ap;							\
									\
	/* Call the user's callback function, if specified. */		\
	va_start(ap);							\
	if ((dbp) != NULL && (dbp)->dbsql_errcall != NULL)		\
		__dbsql_errcall(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
									\
	/* Write to the user's file descriptor, if specified. */	\
	va_start(ap);							\
	if ((dbp) != NULL && (dbp)->dbsql_errfile != NULL)		\
		__dbsql_errfile(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
									\
	/*								\
	 * If we have a default and we didn't do either of the above,	\
	 * write to the default.					\
	 */								\
	va_start(ap);							\
	if ((stderr_default) && ((dbp) == NULL ||			\
	    ((dbp)->dbsql_errcall == NULL && (dbp)->dbsql_errfile == NULL)))\
		__dbsql_errfile(dbp, error, error_set, fmt, ap);	\
	va_end(ap);							\
}
#endif

#if defined(__cplusplus)
}
#endif
#endif /* !_DBSQL_DEBUG_H_ */
