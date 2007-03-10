/* DO NOT EDIT: automatically built by test/scr050/s_include. */
#ifndef	_tcl_ext_h_
#define	_tcl_ext_h_

#if defined(__cplusplus)
extern "C" {
#endif

void __dbsql_tcl_SetErrfile __P((Tcl_Interp *, DBSQL *, DBSQLTCL_INFO *, char *));
int __dbsql_tcl_SetErrpfx __P((Tcl_Interp *, DBSQL *, DBSQLTCL_INFO *, char *));
DBSQLTCL_INFO *__dbsql_tcl_new_info __P((Tcl_Interp *, DBSQL *, char *));
DBSQL *__dbsql_tcl_name_to_ptr __P((CONST char *));
DBSQLTCL_INFO *__dbsql_tcl_ptr_to_info __P((CONST DBSQL *));
DBSQLTCL_INFO *__dbsql_tcl_name_to_info __P((CONST char *));
void  __dbsql_tcl_delete_info __P((DBSQLTCL_INFO *));
void  __dbsql_tcl_set_info_dbp __P((DBSQLTCL_INFO *, void *));
int __dbsql_tcl_return_setup __P((Tcl_Interp *, int, int, char *));
int __dbsql_tcl_error_setup __P((Tcl_Interp *, int, char *));
void __dbsql_tcl_error_func __P((const DBSQL *, CONST char *, const char *));

#if defined(__cplusplus)
}
#endif
#endif /* !_tcl_ext_h_ */
