/* DO NOT EDIT: automatically built by dist/s_include. */
#ifndef	_os_ext_h_
#define	_os_ext_h_

#if defined(__cplusplus)
extern "C" {
#endif

int __os_jtime __P((double *));
void __os_sleep __P((u_long secs, u_long usecs));
int __os_malloc __P((DB_ENV *, size_t, void **));
int __os_realloc __P((DB_ENV *, size_t, void **));
void __os_free __P((DB_ENV *, void *));
int __os_id __P((DB_ENV *, pid_t *, db_threadid_t*));

#if defined(__cplusplus)
}
#endif
#endif /* !_os_ext_h_ */
