/* DO NOT EDIT: automatically built by dist/s_include. */
#ifndef	_clib_ext_h_
#define	_clib_ext_h_

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef HAVE_GETOPT
int getopt __P((int, char * const *, const char *));
#endif
#ifndef HAVE_MEMCMP
int memcmp __P((const void *, const void *, size_t));
#endif
#ifndef HAVE_SRAND48_R
int srand48_r __P((struct drand48_data *));
#endif
int rand8_r __P((struct drand48_data *, u_int8_t *));
int rand32_r __P((struct drand48_data *, u_int32_t *));
#ifndef HAVE_SNPRINTF
int snprintf __P((char *, size_t, const char *, ...));
#endif
#ifndef HAVE_STRCASECMP
int strcasecmp __P((const char *, const char *));
#endif
#ifndef HAVE_STRNCASECMP
int strncasecmp __P((const char *, const char *, size_t));
#endif
#ifndef HAVE_STRDUP
char *strdup __P((const char *));
#endif
#ifndef HAVE_STRNDUP
char *strndup __P((const char *, size_t));
#endif
char *xvprintf __P((DBSQL *, const char *, va_list));
char *xprintf __P((DBSQL *, const char *, ...));

#if defined(__cplusplus)
}
#endif
#endif /* !_clib_ext_h_ */
