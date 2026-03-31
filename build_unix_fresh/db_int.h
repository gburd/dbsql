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

extern int __db_omode __P((const char *));
extern int __os_calloc __P((DB_ENV *, size_t, size_t, void *));
extern int __os_exists __P((DB_ENV *, const char *, int *));
extern int __os_get_errno __P((void));
extern int __os_id  __P((DB_ENV *, pid_t *, db_threadid_t*));
extern int __os_malloc __P((DB_ENV *, size_t, void *));
extern int __os_mkdir __P((DB_ENV *, const char *, int));
extern int __os_realloc __P((DB_ENV *, size_t, void *));
extern int __os_strdup __P((DB_ENV *, const char *, void *));
extern int __os_umalloc __P((DB_ENV *, size_t, void *));
extern int __os_urealloc __P((DB_ENV *, size_t, void *));
extern void *__ua_memcpy __P((void *, const void *, size_t));
extern void __os_free __P((DB_ENV *, void *));
extern void __os_set_errno __P((int));
extern void __os_ufree __P((DB_ENV *, void *));

#if defined(__cplusplus)
}
#endif
#endif /* !_DB_INT_H_ */
