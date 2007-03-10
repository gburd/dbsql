/* DO NOT EDIT: automatically built by dist/s_include. */
#ifndef	_common_ext_h_
#define	_common_ext_h_

#if defined(__cplusplus)
extern "C" {
#endif

int __dbsql_umalloc __P((DBSQL *, size_t, void *));
int __dbsql_urealloc __P((DBSQL *, size_t, void *));
void __dbsql_ufree __P((DBSQL *, void *));
int __dbsql_strdup __P((DBSQL *, const char *, void *));
int __dbsql_strndup __P((DBSQL *, const char *, void *, size_t));
int __dbsql_calloc __P((DBSQL *, size_t, size_t, void *));
int __dbsql_malloc __P((DBSQL *, size_t, void *));
int __dbsql_realloc __P((DBSQL *, size_t, void *));
void __dbsql_free __P((DBSQL *, void *));
double __dbsql_atof __P((const char *));
int __dbsql_atoi __P((const char *, int *));
#ifdef DIAGNOSTIC
void __dbsql_assert __P((const char *, const char *, int));
#endif
int __dbsql_panic_msg __P((DBSQL *));
int __dbsql_panic __P((DBSQL *, int));
void __dbsql_err __P((const DBSQL *, const char *, ...)) __attribute__ ((__format__ (__printf__, 2, 3)));
void __dbsql_errcall __P((const DBSQL *, int, int, const char *, va_list));
void __dbsql_errfile __P((const DBSQL *, int, int, const char *, va_list));
void __error_msg __P((parser_t *, const char *, ...));
void __hash_init __P((hash_t *, int, int));
void __hash_clear __P((hash_t *));
void *__hash_find __P((const hash_t *, const void *, int));
void *__hash_insert __P((hash_t *, const void *, int, void *));
int __hash_ignore_case __P((const char *, int));
void __str_append __P((char **, const char *, ...));
void __str_nappend __P((char **, ...));
void __str_unquote __P((char *));
int __str_urealloc __P((char **));
int __str_is_numeric __P((const char *));
int __str_glob_cmp __P((const unsigned char *, const unsigned char *));
int __str_like_cmp __P((const unsigned char *, const unsigned char *));
int __str_numeric_cmp __P((const char *, const char *));
int __str_int_in32b __P((const char *));
void __str_real_as_sortable __P((double, char *));
int __str_cmp __P((const char *, const char *));

#if defined(__cplusplus)
}
#endif
#endif /* !_common_ext_h_ */
