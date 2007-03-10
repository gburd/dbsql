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
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: db_int.h 7 2007-02-03 13:34:17Z gburd $
 */

#ifndef	_DB_INT_H_
#define	_DB_INT_H_

/*
 * These are function prototypes to non-public API parts of DB we use
 * in DBSQL.  DB may have been compiled with --uniquename, so we have
 * to manage that here.  We also have to smooth out differences from
 * version to version here as internal API is bound to change.
 */

#if defined(__cplusplus)
extern "C" {
#endif

extern void *__ua_memcpy__DB_UNIQUE_NAME__ __P((void *, const void *, size_t));
extern int __os_get_errno__DB_UNIQUE_NAME__ __P((void));
extern void __os_set_errno__DB_UNIQUE_NAME__ __P((int));
extern void __os_sleep__DB_UNIQUE_NAME__ __P((DB_ENV *, u_long, u_long));
extern void __os_free__DB_UNIQUE_NAME__ __P((DB_ENV *, void *));
extern int __os_realloc__DB_UNIQUE_NAME__ __P((DB_ENV *, size_t, void *));
extern int __os_malloc__DB_UNIQUE_NAME__ __P((DB_ENV *, size_t, void *));
extern int __os_calloc__DB_UNIQUE_NAME__ __P((DB_ENV *, size_t, size_t, void *));
extern int __os_strdup__DB_UNIQUE_NAME__ __P((DB_ENV *, const char *, void *));
extern void __os_ufree__DB_UNIQUE_NAME__ __P((DB_ENV *, void *));
extern int __os_urealloc__DB_UNIQUE_NAME__ __P((DB_ENV *, size_t, void *));
extern int __os_umalloc__DB_UNIQUE_NAME__ __P((DB_ENV *, size_t, void *));
extern int __os_exists__DB_UNIQUE_NAME__ __P((const char *, int *));
extern int __db_omode__DB_UNIQUE_NAME__ __P((const char *));

#if defined(__cplusplus)
}
#endif
#endif /* !_DB_INT_H_ */
