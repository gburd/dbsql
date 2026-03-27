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

#include "inc/dbsql_config.h"

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
