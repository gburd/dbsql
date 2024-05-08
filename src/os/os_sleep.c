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
#include <time.h>
#include <unistd.h>
#endif

#include "dbsql_int.h"

/*
 * __os_sleep --
 *      Pause the thread of control.
 *
 * PUBLIC: void __os_sleep __P((u_long secs, u_long usecs));
 */
void
__os_sleep(u_long secs, u_long usecs)
{
    /* Calculate the total time in microseconds */
    u_long total_usecs = secs * 1000000 + usecs;

    /* Convert microseconds to seconds and microseconds */
    u_long sleep_secs = total_usecs / 1000000;
    u_long sleep_usecs = total_usecs % 1000000;

    // Sleep using the standard POSIX sleep function
    struct timespec req;
    struct timespec rem = { 0 };
    req.tv_sec = sleep_secs;
    req.tv_nsec = sleep_usecs * 1000;

    /* Handle potential interruption by signals */
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
}
