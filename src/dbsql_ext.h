/*
** 2006 June 7
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header file defines the dbsql interface for use by
** shared libraries that want to be imported as extensions into
** an dbsql instance.  Shared libraries that intend to be loaded
** as extensions by dbsql should #include this file instead of 
** DBSQL.h.
*/
#ifndef DBSQLEXT_H
#define DBSQLEXT_H
#include "dbsql.h"

/*
** The following structure holds pointers to all of the dbsql API
** routines.
**
** WARNING:  In order to maintain backwards compatibility, add new
** interfaces to the end of this structure only.  If you insert new
** interfaces in the middle of this structure, then older different
** versions of dbsql will not be able to load each other's shared
** libraries!
*/
struct dbsql_api_routines_t {
  void * (*aggregate_context)(dbsql_func_t*,int n_bytes);
  int  (*aggregate_count)(dbsql_func_t*);
  int  (*bind_blob)(dbsql_stmt_t*,int,const void*,int n,void(*)(void*));
  int  (*bind_double)(dbsql_stmt_t*,int,double);
  int  (*bind_int)(dbsql_stmt_t*,int,int);
  int  (*bind_int64)(dbsql_stmt_t*,int,int64_t);
  int  (*bind_null)(dbsql_stmt_t*,int);
  int  (*bind_parameter_count)(dbsql_stmt_t*);
  int  (*bind_parameter_index)(dbsql_stmt_t*,const char*name);
  const char * (*bind_parameter_name)(dbsql_stmt_t*,int);
  int  (*bind_text)(dbsql_stmt_t*,int,const char*,int n,void(*)(void*));
  int  (*bind_text16)(dbsql_stmt_t*,int,const void*,int,void(*)(void*));
  int  (*bind_value)(dbsql_stmt_t*,int,const dbsql_value_t*);
  int  (*busy_handler)(DBSQL*,int(*)(void*,int),void*);
  int  (*busy_timeout)(DBSQL*,int ms);
  int  (*changes)(DBSQL*);
  int  (*close)(DBSQL*);
  int  (*collation_needed)(DBSQL*,void*,void(*)(void*,DBSQL*,
                           int text_rep,const char*));
  int  (*collation_needed16)(DBSQL*,void*,void(*)(void*,DBSQL*,
                             int text_rep,const void*));
  const void * (*column_blob)(dbsql_stmt_t*,int i_col);
  int  (*column_bytes)(dbsql_stmt_t*,int i_col);
  int  (*column_bytes16)(dbsql_stmt_t*,int i_col);
  int  (*column_count)(dbsql_stmt_t*stmt);
  const char * (*column_database_name)(dbsql_stmt_t*,int);
  const void * (*column_database_name16)(dbsql_stmt_t*,int);
  const char * (*column_decltype)(dbsql_stmt_t*,int i);
  const void * (*column_decltype16)(dbsql_stmt_t*,int);
  double  (*column_double)(dbsql_stmt_t*,int i_col);
  int  (*column_int)(dbsql_stmt_t*,int i_col);
  int64_t  (*column_int64)(dbsql_stmt_t*,int i_col);
  const char * (*column_name)(dbsql_stmt_t*,int);
  const void * (*column_name16)(dbsql_stmt_t*,int);
  const char * (*column_origin_name)(dbsql_stmt_t*,int);
  const void * (*column_origin_name16)(dbsql_stmt_t*,int);
  const char * (*column_table_name)(dbsql_stmt_t*,int);
  const void * (*column_table_name16)(dbsql_stmt_t*,int);
  const unsigned char * (*column_text)(dbsql_stmt_t*,int i_col);
  const void * (*column_text16)(dbsql_stmt_t*,int i_col);
  int  (*column_type)(dbsql_stmt_t*,int i_col);
  dbsql_value_t* (*column_value)(dbsql_stmt_t*,int i_col);
  void * (*commit_hook)(DBSQL*,int(*)(void*),void*);
  int  (*complete)(const char*sql);
  int  (*complete16)(const void*sql);
  int  (*create_collation)(DBSQL*,const char*,int,void*,
                           int(*)(void*,int,const void*,int,const void*));
  int  (*create_collation16)(DBSQL*,const void*,int,void*,
                             int(*)(void*,int,const void*,int,const void*));
  int  (*create_function)(DBSQL*,const char*,int,int,void*,
                          void (*func)(dbsql_func_t*,int,dbsql_value_t**),
                          void (*x_step)(dbsql_func_t*,int,dbsql_value_t**),
                          void (*final)(dbsql_func_t*));
  int  (*create_function16)(DBSQL*,const void*,int,int,void*,
                            void (*func)(dbsql_func_t*,int,dbsql_value_t**),
                            void (*x_step)(dbsql_func_t*,int,dbsql_value_t**),
                            void (*final)(dbsql_func_t*));
  int (*create_module)(DBSQL*,const char*,const dbsql_module_t*,void*);
  int  (*data_count)(dbsql_stmt_t*stmt);
  DBSQL * (*db_handle)(dbsql_stmt_t*);
  int (*declare_vtab)(DBSQL*,const char*);
  int  (*enable_shared_cache)(int);
  int  (*errcode)(DBSQL*db);
  const char * (*errmsg)(DBSQL*);
  const void * (*errmsg16)(DBSQL*);
  int  (*exec)(DBSQL*,const char*,dbsql_callback,void*,char**);
  int  (*expired)(dbsql_stmt_t*);
  int  (*finalize)(dbsql_stmt_t*stmt);
  void  (*free)(void*);
  void  (*free_table)(char**result);
  int  (*get_autocommit)(DBSQL*);
  void * (*get_auxdata)(dbsql_func_t*,int);
  int  (*get_table)(DBSQL*,const char*,char***,int*,int*,char**);
  int  (*global_recover)(void);
  void  (*interruptx)(DBSQL*);
  int64_t  (*last_insert_rowid)(DBSQL*);
  const char * (*libversion)(void);
  int  (*libversion_number)(void);
  void *(*malloc)(int);
  char * (*mprintf)(const char*,...);
  int  (*open)(const char*,DBSQL**);
  int  (*open16)(const void*,DBSQL**);
  int  (*prepare)(DBSQL*,const char*,int,dbsql_stmt_t**,const char**);
  int  (*prepare16)(DBSQL*,const void*,int,dbsql_stmt_t**,const void**);
  void * (*profile)(DBSQL*,void(*)(void*,const char*,u_int64_t),void*);
  void  (*progress_handler)(DBSQL*,int,int(*)(void*),void*);
  void *(*realloc)(void*,int);
  int  (*reset)(dbsql_stmt_t*stmt);
  void  (*result_blob)(dbsql_func_t*,const void*,int,void(*)(void*));
  void  (*result_double)(dbsql_func_t*,double);
  void  (*result_error)(dbsql_func_t*,const char*,int);
  void  (*result_error16)(dbsql_func_t*,const void*,int);
  void  (*result_int)(dbsql_func_t*,int);
  void  (*result_int64)(dbsql_func_t*,int64_t);
  void  (*result_null)(dbsql_func_t*);
  void  (*result_text)(dbsql_func_t*,const char*,int,void(*)(void*));
  void  (*result_text16)(dbsql_func_t*,const void*,int,void(*)(void*));
  void  (*result_text16be)(dbsql_func_t*,const void*,int,void(*)(void*));
  void  (*result_text16le)(dbsql_func_t*,const void*,int,void(*)(void*));
  void  (*result_value)(dbsql_func_t*,dbsql_value_t*);
  void * (*rollback_hook)(DBSQL*,void(*)(void*),void*);
  int  (*set_authorizer)(DBSQL*,int(*)(void*,int,const char*,const char*,
                         const char*,const char*),void*);
  void  (*set_auxdata)(dbsql_func_t*,int,void*,void (*)(void*));
  char * (*xsnprintf)(int,char*,const char*,...);
  int  (*step)(dbsql_stmt_t*);
  int  (*table_column_metadata)(DBSQL*,const char*,const char*,const char*,
                                char const**,char const**,int*,int*,int*);
  void  (*thread_cleanup)(void);
  int  (*total_changes)(DBSQL*);
  void * (*trace)(DBSQL*,void(*x_trace)(void*,const char*),void*);
  int  (*transfer_bindings)(dbsql_stmt_t*,dbsql_stmt_t*);
  void * (*update_hook)(DBSQL*,void(*)(void*,int ,char const*,char const*,
                                         int64_t),void*);
  void * (*user_data)(dbsql_func_t*);
  const void * (*value_blob)(dbsql_value_t*);
  int  (*value_bytes)(dbsql_value_t*);
  int  (*value_bytes16)(dbsql_value_t*);
  double  (*value_double)(dbsql_value_t*);
  int  (*value_int)(dbsql_value_t*);
  int64_t  (*value_int64)(dbsql_value_t*);
  int  (*value_numeric_type)(dbsql_value_t*);
  const unsigned char * (*value_text)(dbsql_value_t*);
  const void * (*value_text16)(dbsql_value_t*);
  const void * (*value_text16be)(dbsql_value_t*);
  const void * (*value_text16le)(dbsql_value_t*);
  int  (*value_type)(dbsql_value_t*);
  char *(*vmprintf)(const char*,va_list);
  /* Added ??? */
  int (*overload_function)(DBSQL*, const char *func_name, int n_arg);
  /* Added by 3.3.13 */
  int (*prepare_v2)(DBSQL*,const char*,int,dbsql_stmt_t**,const char**);
  int (*prepare16_v2)(DBSQL*,const void*,int,dbsql_stmt_t**,const void**);
  int (*clear_bindings)(dbsql_stmt_t*);
  /* Added by 3.4.1 */
  int (*create_module_v2)(DBSQL*,const char*,const dbsql_module_t*,void*,
                          void (*destroy)(void *));
  /* Added by 3.5.0 */
  int (*bind_zeroblob)(dbsql_stmt_t*,int,int);
  int (*blob_bytes)(dbsql_blob_t*);
  int (*blob_close)(dbsql_blob_t*);
  int (*blob_open)(DBSQL*,const char*,const char*,const char*,int64_t,
                   int,dbsql_blob_t**);
  int (*blob_read)(dbsql_blob_t*,void*,int,int);
  int (*blob_write)(dbsql_blob_t*,const void*,int,int);
  int (*create_collation_v2)(DBSQL*,const char*,int,void*,
                             int(*)(void*,int,const void*,int,const void*),
                             void(*)(void*));
  int (*file_control)(DBSQL*,const char*,int,void*);
  int64_t (*memory_highwater)(int);
  int64_t (*memory_used)(void);
  dbsql_mutex_t *(*mutex_alloc)(int);
  void (*mutex_enter)(dbsql_mutex_t*);
  void (*mutex_free)(dbsql_mutex_t*);
  void (*mutex_leave)(dbsql_mutex_t*);
  int (*mutex_try)(dbsql_mutex_t*);
  int (*open_v2)(const char*,DBSQL**,int,const char*);
  int (*release_memory)(int);
  void (*result_error_nomem)(dbsql_func_t*);
  void (*result_error_toobig)(dbsql_func_t*);
  int (*sleep)(int);
  void (*soft_heap_limit)(int);
  dbsql_vfs_t *(*vfs_find)(const char*);
  int (*vfs_register)(dbsql_vfs_t*,int);
  int (*vfs_unregister)(dbsql_vfs_t*);
  int (*xthreadsafe)(void);
  void (*result_zeroblob)(dbsql_func_t*,int);
  void (*result_error_code)(dbsql_func_t*,int);
  int (*test_control)(int, ...);
  void (*randomness)(int,void*);
  DBSQL *(*context_db_handle)(dbsql_func_t*);
  int (*extended_result_codes)(DBSQL*,int);
  int (*limit)(DBSQL*,int,int);
  dbsql_stmt_t *(*next_stmt)(DBSQL*,dbsql_stmt_t*);
  const char *(*sql)(dbsql_stmt_t*);
  int (*status)(int,int*,int*,int);
  int (*backup_finish)(dbsql_backup_t*);
  dbsql_backup_t *(*backup_init)(DBSQL*,const char*,DBSQL*,const char*);
  int (*backup_pagecount)(dbsql_backup_t*);
  int (*backup_remaining)(dbsql_backup_t*);
  int (*backup_step)(dbsql_backup_t*,int);
  const char *(*compileoption_get)(int);
  int (*compileoption_used)(const char*);
  int (*create_function_v2)(DBSQL*,const char*,int,int,void*,
                            void (*func)(dbsql_func_t*,int,dbsql_value_t**),
                            void (*x_step)(dbsql_func_t*,int,dbsql_value_t**),
                            void (*final)(dbsql_func_t*),
                            void(*destroy)(void*));
  int (*db_config)(DBSQL*,int,...);
  dbsql_mutex_t *(*db_mutex)(DBSQL*);
  int (*db_status)(DBSQL*,int,int*,int*,int);
  int (*extended_errcode)(DBSQL*);
  void (*log)(int,const char*,...);
  int64_t (*soft_heap_limit64)(int64_t);
  const char *(*sourceid)(void);
  int (*stmt_status)(dbsql_stmt_t*,int,int);
  int (*strnicmp)(const char*,const char*,int);
  int (*unlock_notify)(DBSQL*,void(*)(void**,int),void*);
  int (*wal_autocheckpoint)(DBSQL*,int);
  int (*wal_checkpoint)(DBSQL*,const char*);
  void *(*wal_hook)(DBSQL*,int(*)(void*,DBSQL*,const char*,int),void*);
  int (*blob_reopen)(dbsql_blob_t*,int64_t);
  int (*vtab_config)(DBSQL*,int op,...);
  int (*vtab_on_conflict)(DBSQL*);
  /* Version 3.7.16 and later */
  int (*close_v2)(DBSQL*);
  const char *(*db_filename)(DBSQL*,const char*);
  int (*db_readonly)(DBSQL*,const char*);
  int (*db_release_memory)(DBSQL*);
  const char *(*errstr)(int);
  int (*stmt_busy)(dbsql_stmt_t*);
  int (*stmt_readonly)(dbsql_stmt_t*);
  int (*stricmp)(const char*,const char*);
  int (*uri_boolean)(const char*,const char*,int);
  int64_t (*uri_int64)(const char*,const char*,int64_t);
  const char *(*uri_parameter)(const char*,const char*);
  char *(*xvsnprintf)(int,char*,const char*,va_list);
  int (*wal_checkpoint_v2)(DBSQL*,const char*,int,int*,int*);
  /* Version 3.8.7 and later */
  int (*auto_extension)(void(*)(void));
  int (*bind_blob64)(dbsql_stmt_t*,int,const void*,u_int64_t,
                     void(*)(void*));
  int (*bind_text64)(dbsql_stmt_t*,int,const char*,u_int64_t,
                      void(*)(void*),unsigned char);
  int (*cancel_auto_extension)(void(*)(void));
  int (*load_extension)(DBSQL*,const char*,const char*,char**);
  void *(*malloc64)(u_int64_t);
  u_int64_t (*msize)(void*);
  void *(*realloc64)(void*,u_int64_t);
  void (*reset_auto_extension)(void);
  void (*result_blob64)(dbsql_func_t*,const void*,u_int64_t,
                        void(*)(void*));
  void (*result_text64)(dbsql_func_t*,const char*,u_int64_t,
                         void(*)(void*), unsigned char);
  int (*strglob)(const char*,const char*);
  /* Version 3.8.11 and later */
  dbsql_value_t *(*value_dup)(const dbsql_value_t*);
  void (*value_free)(dbsql_value_t*);
  int (*result_zeroblob64)(dbsql_func_t*,u_int64_t);
  int (*bind_zeroblob64)(dbsql_stmt_t*, int, u_int64_t);
  /* Version 3.9.0 and later */
  unsigned int (*value_subtype)(dbsql_value_t*);
  void (*result_subtype)(dbsql_func_t*,unsigned int);
  /* Version 3.10.0 and later */
  int (*status64)(int,int64_t*,int64_t*,int);
  int (*strlike)(const char*,const char*,unsigned int);
  int (*db_cacheflush)(DBSQL*);
  /* Version 3.12.0 and later */
  int (*system_errno)(DBSQL*);
  /* Version 3.14.0 and later */
  int (*trace_v2)(DBSQL*,unsigned,int(*)(unsigned,void*,void*,void*),void*);
  char *(*expanded_sql)(dbsql_stmt_t*);
  /* Version 3.18.0 and later */
  void (*set_last_insert_rowid)(DBSQL*,int64_t);
  /* Version 3.20.0 and later */
  int (*prepare_v3)(DBSQL*,const char*,int,unsigned int,
                    dbsql_stmt_t**,const char**);
  int (*prepare16_v3)(DBSQL*,const void*,int,unsigned int,
                      dbsql_stmt_t**,const void**);
  int (*bind_pointer)(dbsql_stmt_t*,int,void*,const char*,void(*)(void*));
  void (*result_pointer)(dbsql_func_t*,void*,const char*,void(*)(void*));
  void *(*value_pointer)(dbsql_value_t*,const char*);
  int (*vtab_nochange)(dbsql_func_t*);
  int (*value_nochange)(dbsql_value_t*);
  const char *(*vtab_collation)(dbsql_index_info_t*,int);
  /* Version 3.24.0 and later */
  int (*keyword_count)(void);
  int (*keyword_name)(int,const char**,int*);
  int (*keyword_check)(const char*,int);
  dbsql_str_t *(*str_new)(DBSQL*);
  char *(*str_finish)(dbsql_str_t*);
  void (*str_appendf)(dbsql_str_t*, const char *format, ...);
  void (*str_vappendf)(dbsql_str_t*, const char *format, va_list);
  void (*str_append)(dbsql_str_t*, const char *z_in, int N);
  void (*str_appendall)(dbsql_str_t*, const char *in);
  void (*str_appendchar)(dbsql_str_t*, int N, char C);
  void (*str_reset)(dbsql_str_t*);
  int (*str_errcode)(dbsql_str_t*);
  int (*str_length)(dbsql_str_t*);
  char *(*str_value)(dbsql_str_t*);
  /* Version 3.25.0 and later */
  int (*create_window_function)(DBSQL*,const char*,int,int,void*,
                            void (*x_step)(dbsql_func_t*,int,dbsql_value_t**),
                            void (*final)(dbsql_func_t*),
                            void (*value)(dbsql_func_t*),
                            void (*inv)(dbsql_func_t*,int,dbsql_value_t**),
                            void(*destroy)(void*));
  /* Version 3.26.0 and later */
  const char *(*normalized_sql)(dbsql_stmt_t*);
  /* Version 3.28.0 and later */
  int (*stmt_isexplain)(dbsql_stmt_t*);
  int (*value_frombind)(dbsql_value_t*);
  /* Version 3.30.0 and later */
  int (*drop_modules)(DBSQL*,const char**);
  /* Version 3.31.0 and later */
  int64_t (*hard_heap_limit64)(int64_t);
  const char *(*uri_key)(const char*,int);
  const char *(*filename_database)(const char*);
  const char *(*filename_journal)(const char*);
  const char *(*filename_wal)(const char*);
  /* Version 3.32.0 and later */
  const char *(*create_filename)(const char*,const char*,const char*,
                           int,const char**);
  void (*free_filename)(const char*);
  dbsql_file_t *(*database_file_object)(const char*);
  /* Version 3.34.0 and later */
  int (*txn_state)(DBSQL*,const char*);
  /* Version 3.36.1 and later */
  int64_t (*changes64)(DBSQL*);
  int64_t (*total_changes64)(DBSQL*);
  /* Version 3.37.0 and later */
  int (*autovacuum_pages)(DBSQL*,
     unsigned int(*)(void*,const char*,unsigned int,unsigned int,unsigned int),
     void*, void(*)(void*));
  /* Version 3.38.0 and later */
  int (*error_offset)(DBSQL*);
  int (*vtab_rhs_value)(dbsql_index_info_t*,int,dbsql_value_t**);
  int (*vtab_distinct)(dbsql_index_info_t*);
  int (*vtab_in)(dbsql_index_info_t*,int,int);
  int (*vtab_in_first)(dbsql_value_t*,dbsql_value_t**);
  int (*vtab_in_next)(dbsql_value_t*,dbsql_value_t**);
  /* Version 3.39.0 and later */
  int (*deserialize)(DBSQL*,const char*,unsigned char*,
                     int64_t,int64_t,unsigned);
  unsigned char *(*serialize)(DBSQL*,const char *,int64_t*,
                              unsigned int);
  const char *(*db_name)(DBSQL*,int);
  /* Version 3.40.0 and later */
  int (*value_encoding)(dbsql_value_t*);
  /* Version 3.41.0 and later */
  int (*is_interrupted)(DBSQL*);
  /* Version 3.43.0 and later */
  int (*stmt_explain)(dbsql_stmt_t*,int);
  /* Version 3.44.0 and later */
  void *(*get_clientdata)(DBSQL*,const char*);
  int (*set_clientdata)(DBSQL*, const char*, void*, void(*)(void*));
  /* Version 3.50.0 and later */
  int (*setlk_timeout)(DBSQL*,int,int);
  /* Version 3.51.0 and later */
  int (*set_errmsg)(DBSQL*,int,const char*);
  int (*db_status64)(DBSQL*,int,int64_t*,int64_t*,int);
  /* Version 3.52.0 and later */
  void (*str_truncate)(dbsql_str_t*,int);
  void (*str_free)(dbsql_str_t*);
  int (*carray_bind)(dbsql_stmt_t*,int,void*,int,int,void(*)(void*));
  int (*carray_bind_v2)(dbsql_stmt_t*,int,void*,int,int,void(*)(void*),void*);
};

/*
** This is the function signature used for all extension entry points.  It
** is also defined in the file "loadext.c".
*/
typedef int (*dbsql_loadext_entry)(
  DBSQL *db,                       /* Handle to the database. */
  char **errmsg_p,                   /* Used to set error string on failure. */
  const dbsql_api_routines_t *thunk /* Extension API function pointers. */
);

/*
** The following macros redefine the API routines so that they are
** redirected through the global dbsql_api structure.
**
** This header file is also used by the loadext.c source file
** (part of the main dbsql library - not an extension) so that
** it can get access to the dbsql_api_routines_t structure
** definition.  But the main library does not want to redefine
** the API.  So the redefinition macros are only valid if the
** DBSQL_CORE macros is undefined.
*/
#if !defined(DBSQL_CORE) && !defined(DBSQL_OMIT_LOAD_EXTENSION)
#define dbsql_aggregate_context      dbsql_api->aggregate_context
#ifndef DBSQL_OMIT_DEPRECATED
#define dbsql_aggregate_count        dbsql_api->aggregate_count
#endif
#define dbsql_bind_blob              dbsql_api->bind_blob
#define dbsql_bind_double            dbsql_api->bind_double
#define dbsql_bind_int               dbsql_api->bind_int
#define dbsql_bind_int64             dbsql_api->bind_int64
#define dbsql_bind_null              dbsql_api->bind_null
#define dbsql_bind_parameter_count   dbsql_api->bind_parameter_count
#define dbsql_bind_parameter_index   dbsql_api->bind_parameter_index
#define dbsql_bind_parameter_name    dbsql_api->bind_parameter_name
#define dbsql_bind_text              dbsql_api->bind_text
#define dbsql_bind_text16            dbsql_api->bind_text16
#define dbsql_bind_value             dbsql_api->bind_value
#define dbsql_busy_handler           dbsql_api->busy_handler
#define dbsql_busy_timeout           dbsql_api->busy_timeout
#define dbsql_changes                dbsql_api->changes
#define dbsql_close                  dbsql_api->close
#define dbsql_collation_needed       dbsql_api->collation_needed
#define dbsql_collation_needed16     dbsql_api->collation_needed16
#define dbsql_column_blob            dbsql_api->column_blob
#define dbsql_column_bytes           dbsql_api->column_bytes
#define dbsql_column_bytes16         dbsql_api->column_bytes16
#define dbsql_column_count           dbsql_api->column_count
#define dbsql_column_database_name   dbsql_api->column_database_name
#define dbsql_column_database_name16 dbsql_api->column_database_name16
#define dbsql_column_decltype        dbsql_api->column_decltype
#define dbsql_column_decltype16      dbsql_api->column_decltype16
#define dbsql_column_double          dbsql_api->column_double
#define dbsql_column_int             dbsql_api->column_int
#define dbsql_column_int64           dbsql_api->column_int64
#define dbsql_column_name            dbsql_api->column_name
#define dbsql_column_name16          dbsql_api->column_name16
#define dbsql_column_origin_name     dbsql_api->column_origin_name
#define dbsql_column_origin_name16   dbsql_api->column_origin_name16
#define dbsql_column_table_name      dbsql_api->column_table_name
#define dbsql_column_table_name16    dbsql_api->column_table_name16
#define dbsql_column_text            dbsql_api->column_text
#define dbsql_column_text16          dbsql_api->column_text16
#define dbsql_column_type            dbsql_api->column_type
#define dbsql_column_value           dbsql_api->column_value
#define dbsql_commit_hook            dbsql_api->commit_hook
#define dbsql_complete               dbsql_api->complete
#define dbsql_complete16             dbsql_api->complete16
#define dbsql_create_collation       dbsql_api->create_collation
#define dbsql_create_collation16     dbsql_api->create_collation16
#define dbsql_create_function        dbsql_api->create_function
#define dbsql_create_function16      dbsql_api->create_function16
#define dbsql_create_module          dbsql_api->create_module
#define dbsql_create_module_v2       dbsql_api->create_module_v2
#define dbsql_data_count             dbsql_api->data_count
#define dbsql_db_handle              dbsql_api->db_handle
#define dbsql_declare_vtab           dbsql_api->declare_vtab
#define dbsql_enable_shared_cache    dbsql_api->enable_shared_cache
#define dbsql_errcode                dbsql_api->errcode
#define dbsql_errmsg                 dbsql_api->errmsg
#define dbsql_errmsg16               dbsql_api->errmsg16
#define dbsql_exec                   dbsql_api->exec
#ifndef DBSQL_OMIT_DEPRECATED
#define dbsql_expired                dbsql_api->expired
#endif
#define dbsql_finalize               dbsql_api->finalize
#define dbsql_free                   dbsql_api->free
#define dbsql_free_table             dbsql_api->free_table
#define dbsql_get_autocommit         dbsql_api->get_autocommit
#define dbsql_get_auxdata            dbsql_api->get_auxdata
#define dbsql_get_table              dbsql_api->get_table
#ifndef DBSQL_OMIT_DEPRECATED
#define dbsql_global_recover         dbsql_api->global_recover
#endif
#define dbsql_interrupt              dbsql_api->interruptx
#define dbsql_last_insert_rowid      dbsql_api->last_insert_rowid
#define dbsql_libversion             dbsql_api->libversion
#define dbsql_libversion_number      dbsql_api->libversion_number
#define dbsql_malloc                 dbsql_api->malloc
#define dbsql_mprintf                dbsql_api->mprintf
#define dbsql_open                   dbsql_api->open
#define dbsql_open16                 dbsql_api->open16
#define dbsql_prepare                dbsql_api->prepare
#define dbsql_prepare16              dbsql_api->prepare16
#define dbsql_prepare_v2             dbsql_api->prepare_v2
#define dbsql_prepare16_v2           dbsql_api->prepare16_v2
#define dbsql_profile                dbsql_api->profile
#define dbsql_progress_handler       dbsql_api->progress_handler
#define dbsql_realloc                dbsql_api->realloc
#define dbsql_reset                  dbsql_api->reset
#define dbsql_result_blob            dbsql_api->result_blob
#define dbsql_result_double          dbsql_api->result_double
#define dbsql_result_error           dbsql_api->result_error
#define dbsql_result_error16         dbsql_api->result_error16
#define dbsql_result_int             dbsql_api->result_int
#define dbsql_result_int64           dbsql_api->result_int64
#define dbsql_result_null            dbsql_api->result_null
#define dbsql_result_text            dbsql_api->result_text
#define dbsql_result_text16          dbsql_api->result_text16
#define dbsql_result_text16be        dbsql_api->result_text16be
#define dbsql_result_text16le        dbsql_api->result_text16le
#define dbsql_result_value           dbsql_api->result_value
#define dbsql_rollback_hook          dbsql_api->rollback_hook
#define dbsql_set_authorizer         dbsql_api->set_authorizer
#define dbsql_set_auxdata            dbsql_api->set_auxdata
#define dbsql_snprintf               dbsql_api->xsnprintf
#define dbsql_step                   dbsql_api->step
#define dbsql_table_column_metadata  dbsql_api->table_column_metadata
#define dbsql_thread_cleanup         dbsql_api->thread_cleanup
#define dbsql_total_changes          dbsql_api->total_changes
#define dbsql_trace                  dbsql_api->trace
#ifndef DBSQL_OMIT_DEPRECATED
#define dbsql_transfer_bindings      dbsql_api->transfer_bindings
#endif
#define dbsql_update_hook            dbsql_api->update_hook
#define dbsql_user_data              dbsql_api->user_data
#define dbsql_value_blob             dbsql_api->value_blob
#define dbsql_value_bytes            dbsql_api->value_bytes
#define dbsql_value_bytes16          dbsql_api->value_bytes16
#define dbsql_value_double           dbsql_api->value_double
#define dbsql_value_int              dbsql_api->value_int
#define dbsql_value_int64            dbsql_api->value_int64
#define dbsql_value_numeric_type     dbsql_api->value_numeric_type
#define dbsql_value_text             dbsql_api->value_text
#define dbsql_value_text16           dbsql_api->value_text16
#define dbsql_value_text16be         dbsql_api->value_text16be
#define dbsql_value_text16le         dbsql_api->value_text16le
#define dbsql_value_type             dbsql_api->value_type
#define dbsql_vmprintf               dbsql_api->vmprintf
#define dbsql_vsnprintf              dbsql_api->xvsnprintf
#define dbsql_overload_function      dbsql_api->overload_function
#define dbsql_prepare_v2             dbsql_api->prepare_v2
#define dbsql_prepare16_v2           dbsql_api->prepare16_v2
#define dbsql_clear_bindings         dbsql_api->clear_bindings
#define dbsql_bind_zeroblob          dbsql_api->bind_zeroblob
#define dbsql_blob_bytes             dbsql_api->blob_bytes
#define dbsql_blob_close             dbsql_api->blob_close
#define dbsql_blob_open              dbsql_api->blob_open
#define dbsql_blob_read              dbsql_api->blob_read
#define dbsql_blob_write             dbsql_api->blob_write
#define dbsql_create_collation_v2    dbsql_api->create_collation_v2
#define dbsql_file_control           dbsql_api->file_control
#define dbsql_memory_highwater       dbsql_api->memory_highwater
#define dbsql_memory_used            dbsql_api->memory_used
#define dbsql_mutex_alloc            dbsql_api->mutex_alloc
#define dbsql_mutex_enter            dbsql_api->mutex_enter
#define dbsql_mutex_free             dbsql_api->mutex_free
#define dbsql_mutex_leave            dbsql_api->mutex_leave
#define dbsql_mutex_try              dbsql_api->mutex_try
#define dbsql_open_v2                dbsql_api->open_v2
#define dbsql_release_memory         dbsql_api->release_memory
#define dbsql_result_error_nomem     dbsql_api->result_error_nomem
#define dbsql_result_error_toobig    dbsql_api->result_error_toobig
#define dbsql_sleep                  dbsql_api->sleep
#define dbsql_soft_heap_limit        dbsql_api->soft_heap_limit
#define dbsql_vfs_find               dbsql_api->vfs_find
#define dbsql_vfs_register           dbsql_api->vfs_register
#define dbsql_vfs_unregister         dbsql_api->vfs_unregister
#define dbsql_threadsafe             dbsql_api->xthreadsafe
#define dbsql_result_zeroblob        dbsql_api->result_zeroblob
#define dbsql_result_error_code      dbsql_api->result_error_code
#define dbsql_test_control           dbsql_api->test_control
#define dbsql_randomness             dbsql_api->randomness
#define dbsql_context_db_handle      dbsql_api->context_db_handle
#define dbsql_extended_result_codes  dbsql_api->extended_result_codes
#define dbsql_limit                  dbsql_api->limit
#define dbsql_next_stmt              dbsql_api->next_stmt
#define dbsql_sql                    dbsql_api->sql
#define dbsql_status                 dbsql_api->status
#define dbsql_backup_finish          dbsql_api->backup_finish
#define dbsql_backup_init            dbsql_api->backup_init
#define dbsql_backup_pagecount       dbsql_api->backup_pagecount
#define dbsql_backup_remaining       dbsql_api->backup_remaining
#define dbsql_backup_step            dbsql_api->backup_step
#define dbsql_compileoption_get      dbsql_api->compileoption_get
#define dbsql_compileoption_used     dbsql_api->compileoption_used
#define dbsql_create_function_v2     dbsql_api->create_function_v2
#define dbsql_db_config              dbsql_api->db_config
#define dbsql_db_mutex               dbsql_api->db_mutex
#define dbsql_db_status              dbsql_api->db_status
#define dbsql_extended_errcode       dbsql_api->extended_errcode
#define dbsql_log                    dbsql_api->log
#define dbsql_soft_heap_limit64      dbsql_api->soft_heap_limit64
#define dbsql_sourceid               dbsql_api->sourceid
#define dbsql_stmt_status            dbsql_api->stmt_status
#define dbsql_strnicmp               dbsql_api->strnicmp
#define dbsql_unlock_notify          dbsql_api->unlock_notify
#define dbsql_wal_autocheckpoint     dbsql_api->wal_autocheckpoint
#define dbsql_wal_checkpoint         dbsql_api->wal_checkpoint
#define dbsql_wal_hook               dbsql_api->wal_hook
#define dbsql_blob_reopen            dbsql_api->blob_reopen
#define dbsql_vtab_config            dbsql_api->vtab_config
#define dbsql_vtab_on_conflict       dbsql_api->vtab_on_conflict
/* Version 3.7.16 and later */
#define dbsql_close_v2               dbsql_api->close_v2
#define dbsql_db_filename            dbsql_api->db_filename
#define dbsql_db_readonly            dbsql_api->db_readonly
#define dbsql_db_release_memory      dbsql_api->db_release_memory
#define dbsql_errstr                 dbsql_api->errstr
#define dbsql_stmt_busy              dbsql_api->stmt_busy
#define dbsql_stmt_readonly          dbsql_api->stmt_readonly
#define dbsql_stricmp                dbsql_api->stricmp
#define dbsql_uri_boolean            dbsql_api->uri_boolean
#define dbsql_uri_int64              dbsql_api->uri_int64
#define dbsql_uri_parameter          dbsql_api->uri_parameter
#define dbsql_uri_vsnprintf          dbsql_api->xvsnprintf
#define dbsql_wal_checkpoint_v2      dbsql_api->wal_checkpoint_v2
/* Version 3.8.7 and later */
#define dbsql_auto_extension         dbsql_api->auto_extension
#define dbsql_bind_blob64            dbsql_api->bind_blob64
#define dbsql_bind_text64            dbsql_api->bind_text64
#define dbsql_cancel_auto_extension  dbsql_api->cancel_auto_extension
#define dbsql_load_extension         dbsql_api->load_extension
#define dbsql_malloc64               dbsql_api->malloc64
#define dbsql_msize                  dbsql_api->msize
#define dbsql_realloc64              dbsql_api->realloc64
#define dbsql_reset_auto_extension   dbsql_api->reset_auto_extension
#define dbsql_result_blob64          dbsql_api->result_blob64
#define dbsql_result_text64          dbsql_api->result_text64
#define dbsql_strglob                dbsql_api->strglob
/* Version 3.8.11 and later */
#define dbsql_value_dup              dbsql_api->value_dup
#define dbsql_value_free             dbsql_api->value_free
#define dbsql_result_zeroblob64      dbsql_api->result_zeroblob64
#define dbsql_bind_zeroblob64        dbsql_api->bind_zeroblob64
/* Version 3.9.0 and later */
#define dbsql_value_subtype          dbsql_api->value_subtype
#define dbsql_result_subtype         dbsql_api->result_subtype
/* Version 3.10.0 and later */
#define dbsql_status64               dbsql_api->status64
#define dbsql_strlike                dbsql_api->strlike
#define dbsql_db_cacheflush          dbsql_api->db_cacheflush
/* Version 3.12.0 and later */
#define dbsql_system_errno           dbsql_api->system_errno
/* Version 3.14.0 and later */
#define dbsql_trace_v2               dbsql_api->trace_v2
#define dbsql_expanded_sql           dbsql_api->expanded_sql
/* Version 3.18.0 and later */
#define dbsql_set_last_insert_rowid  dbsql_api->set_last_insert_rowid
/* Version 3.20.0 and later */
#define dbsql_prepare_v3             dbsql_api->prepare_v3
#define dbsql_prepare16_v3           dbsql_api->prepare16_v3
#define dbsql_bind_pointer           dbsql_api->bind_pointer
#define dbsql_result_pointer         dbsql_api->result_pointer
#define dbsql_value_pointer          dbsql_api->value_pointer
/* Version 3.22.0 and later */
#define dbsql_vtab_nochange          dbsql_api->vtab_nochange
#define dbsql_value_nochange         dbsql_api->value_nochange
#define dbsql_vtab_collation         dbsql_api->vtab_collation
/* Version 3.24.0 and later */
#define dbsql_keyword_count          dbsql_api->keyword_count
#define dbsql_keyword_name           dbsql_api->keyword_name
#define dbsql_keyword_check          dbsql_api->keyword_check
#define dbsql_str_new                dbsql_api->str_new
#define dbsql_str_finish             dbsql_api->str_finish
#define dbsql_str_appendf            dbsql_api->str_appendf
#define dbsql_str_vappendf           dbsql_api->str_vappendf
#define dbsql_str_append             dbsql_api->str_append
#define dbsql_str_appendall          dbsql_api->str_appendall
#define dbsql_str_appendchar         dbsql_api->str_appendchar
#define dbsql_str_reset              dbsql_api->str_reset
#define dbsql_str_errcode            dbsql_api->str_errcode
#define dbsql_str_length             dbsql_api->str_length
#define dbsql_str_value              dbsql_api->str_value
/* Version 3.25.0 and later */
#define dbsql_create_window_function dbsql_api->create_window_function
/* Version 3.26.0 and later */
#define dbsql_normalized_sql         dbsql_api->normalized_sql
/* Version 3.28.0 and later */
#define dbsql_stmt_isexplain         dbsql_api->stmt_isexplain
#define dbsql_value_frombind         dbsql_api->value_frombind
/* Version 3.30.0 and later */
#define dbsql_drop_modules           dbsql_api->drop_modules
/* Version 3.31.0 and later */
#define dbsql_hard_heap_limit64      dbsql_api->hard_heap_limit64
#define dbsql_uri_key                dbsql_api->uri_key
#define dbsql_filename_database      dbsql_api->filename_database
#define dbsql_filename_journal       dbsql_api->filename_journal
#define dbsql_filename_wal           dbsql_api->filename_wal
/* Version 3.32.0 and later */
#define dbsql_create_filename        dbsql_api->create_filename
#define dbsql_free_filename          dbsql_api->free_filename
#define dbsql_database_file_object   dbsql_api->database_file_object
/* Version 3.34.0 and later */
#define dbsql_txn_state              dbsql_api->txn_state
/* Version 3.36.1 and later */
#define dbsql_changes64              dbsql_api->changes64
#define dbsql_total_changes64        dbsql_api->total_changes64
/* Version 3.37.0 and later */
#define dbsql_autovacuum_pages       dbsql_api->autovacuum_pages
/* Version 3.38.0 and later */
#define dbsql_error_offset           dbsql_api->error_offset
#define dbsql_vtab_rhs_value         dbsql_api->vtab_rhs_value
#define dbsql_vtab_distinct          dbsql_api->vtab_distinct
#define dbsql_vtab_in                dbsql_api->vtab_in
#define dbsql_vtab_in_first          dbsql_api->vtab_in_first
#define dbsql_vtab_in_next           dbsql_api->vtab_in_next
/* Version 3.39.0 and later */
#ifndef DBSQL_OMIT_DESERIALIZE
#define dbsql_deserialize            dbsql_api->deserialize
#define dbsql_serialize              dbsql_api->serialize
#endif
#define dbsql_db_name                dbsql_api->db_name
/* Version 3.40.0 and later */
#define dbsql_value_encoding         dbsql_api->value_encoding
/* Version 3.41.0 and later */
#define dbsql_is_interrupted         dbsql_api->is_interrupted
/* Version 3.43.0 and later */
#define dbsql_stmt_explain           dbsql_api->stmt_explain
/* Version 3.44.0 and later */
#define dbsql_get_clientdata         dbsql_api->get_clientdata
#define dbsql_set_clientdata         dbsql_api->set_clientdata
/* Version 3.50.0 and later */
#define dbsql_setlk_timeout          dbsql_api->setlk_timeout
/* Version 3.51.0 and later */
#define dbsql_set_errmsg             dbsql_api->set_errmsg
#define dbsql_db_status64            dbsql_api->db_status64
/* Version 3.52.0 and later */
#define dbsql_str_truncate           dbsql_api->str_truncate
#define dbsql_str_free               dbsql_api->str_free
#define dbsql_carray_bind            dbsql_api->carray_bind
#define dbsql_carray_bind_v2         dbsql_api->carray_bind_v2
#endif /* !defined(DBSQL_CORE) && !defined(DBSQL_OMIT_LOAD_EXTENSION) */

#if !defined(DBSQL_CORE) && !defined(DBSQL_OMIT_LOAD_EXTENSION)
  /* This case when the file really is being compiled as a loadable 
  ** extension */
# define DBSQL_EXTENSION_INIT1     const dbsql_api_routines_t *dbsql_api=0;
# define DBSQL_EXTENSION_INIT2(v)  dbsql_api=v;
# define DBSQL_EXTENSION_INIT3     \
    extern const dbsql_api_routines_t *dbsql_api;
#else
  /* This case when the file is being statically linked into the 
  ** application */
# define DBSQL_EXTENSION_INIT1     /*no-op*/
# define DBSQL_EXTENSION_INIT2(v)  (void)v; /* unused parameter */
# define DBSQL_EXTENSION_INIT3     /*no-op*/
#endif

#endif /* DBSQLEXT_H */
