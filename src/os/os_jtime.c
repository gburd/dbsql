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
 * $Id: os_jtime.c 7 2007-02-03 13:34:17Z gburd $
 */

#include "dbsql_config.h"

#if DB_WIN32
#include <winbase.h>
#endif

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdio.h>
#endif

#include "dbsql_int.h"

/*
 * The following variable, if set to a now-zero value, become the result
 * returned from __os_current_time().  This is used for testing only.
 */
#ifdef CONFIG_TEST
int _fake_current_time = 0;
#endif

/*
 * __os_jtime --
 *	The Julian time in UTC now.
 *
 * PUBLIC: int __os_jtime __P((double *));
 */
int
__os_jtime(result)
	double *result;
{
#ifndef DB_WIN32
  time_t t;
  time(&t);
  *result = t / 86400.0 + 2440587.5;
#else /* DB_WIN32 */
  /*
   * FILETIME structure is a 64-bit value representing the number of 
   * 100-nanosecond intervals since January 1, 1601 (= JD 2305813.5). 
   */
  FILETIME ft;
  double now;
  GetSystemTimeAsFileTime(&ft);
  now = ((double)ft.dwHighDateTime) * 4294967296.0; 
  *result = (now + ft.dwLowDateTime) / 864000000000.0 + 2305813.5;
#endif
#ifdef CONFIG_TEST
  if (_fake_current_time) {
	  *result = _fake_current_time / 86400.0 + 2440587.5;
  }
#endif
  return DBSQL_SUCCESS;
}
