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
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * http://creativecommons.org/licenses/GPL/2.0/
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
