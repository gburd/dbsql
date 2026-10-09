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
** This file contains code used to dynamically load extensions into
** the dbsql library.
*/

#ifndef DBSQL_CORE
  #define DBSQL_CORE 1  /* Disable the API redefinition in sqlite3ext.h */
#endif
#include "dbsql_ext.h"
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_LOAD_EXTENSION
/*
** Some API routines are omitted when various features are
** excluded from a build of dbsql.  Substitute a NULL pointer
** for any missing APIs.
*/
#ifndef DBSQL_ENABLE_COLUMN_METADATA
# define dbsql_column_database_name   0
# define dbsql_column_database_name16 0
# define dbsql_column_table_name      0
# define dbsql_column_table_name16    0
# define dbsql_column_origin_name     0
# define dbsql_column_origin_name16   0
#endif

#ifdef DBSQL_OMIT_AUTHORIZATION
# define dbsql_set_authorizer         0
#endif

#ifdef DBSQL_OMIT_UTF16
# define dbsql_bind_text16            0
# define dbsql_collation_needed16     0
# define dbsql_column_decltype16      0
# define dbsql_column_name16          0
# define dbsql_column_text16          0
# define dbsql_complete16             0
# define dbsql_create_collation16     0
# define dbsql_create_function16      0
# define dbsql_errmsg16               0
# define dbsql_open16                 0
# define dbsql_prepare16              0
# define dbsql_prepare16_v2           0
# define dbsql_prepare16_v3           0
# define dbsql_result_error16         0
# define dbsql_result_text16          0
# define dbsql_result_text16be        0
# define dbsql_result_text16le        0
# define dbsql_value_text16           0
# define dbsql_value_text16be         0
# define dbsql_value_text16le         0
# define dbsql_column_database_name16 0
# define dbsql_column_table_name16    0
# define dbsql_column_origin_name16   0
#endif

#ifdef DBSQL_OMIT_COMPLETE
# define dbsql_complete 0
# define dbsql_complete16 0
#endif

#ifdef DBSQL_OMIT_DECLTYPE
# define dbsql_column_decltype16      0
# define dbsql_column_decltype        0
#endif

#ifdef DBSQL_OMIT_PROGRESS_CALLBACK
# define dbsql_progress_handler 0
#endif

#ifdef DBSQL_OMIT_VIRTUALTABLE
# define dbsql_create_module 0
# define dbsql_create_module_v2 0
# define dbsql_declare_vtab 0
# define dbsql_vtab_config 0
# define dbsql_vtab_on_conflict 0
# define dbsql_vtab_collation 0
#endif

#ifdef DBSQL_OMIT_SHARED_CACHE
# define dbsql_enable_shared_cache 0
#endif

#if defined(DBSQL_OMIT_TRACE) || defined(DBSQL_OMIT_DEPRECATED)
# define dbsql_profile       0
# define dbsql_trace         0
#endif

#ifdef DBSQL_OMIT_GET_TABLE
# define dbsql_free_table    0
# define dbsql_get_table     0
#endif

#ifdef DBSQL_OMIT_INCRBLOB
#define dbsql_bind_zeroblob  0
#define dbsql_blob_bytes     0
#define dbsql_blob_close     0
#define dbsql_blob_open      0
#define dbsql_blob_read      0
#define dbsql_blob_write     0
#define dbsql_blob_reopen    0
#endif

#if defined(DBSQL_OMIT_TRACE)
# define dbsql_trace_v2      0
#endif

/*
** The following structure contains pointers to all dbsql API routines.
** A pointer to this structure is passed into extensions when they are
** loaded so that the extension can make calls back into the dbsql
** library.
**
** When adding new APIs, add them to the bottom of this structure
** in order to preserve backwards compatibility.
**
** Extensions that use newer APIs should first call the
** dbsql_libversion_number() to make sure that the API they
** intend to use is supported by the library.  Extensions should
** also check to make sure that the pointer to the function is
** not NULL before calling it.
*/
static const dbsql_api_routines_t __apis = {
  dbsql_aggregate_context,
#ifndef DBSQL_OMIT_DEPRECATED
  dbsql_aggregate_count,
#else
  0,
#endif
  dbsql_bind_blob,
  dbsql_bind_double,
  dbsql_bind_int,
  dbsql_bind_int64,
  dbsql_bind_null,
  dbsql_bind_parameter_count,
  dbsql_bind_parameter_index,
  dbsql_bind_parameter_name,
  dbsql_bind_text,
  dbsql_bind_text16,
  dbsql_bind_value,
  dbsql_busy_handler,
  dbsql_busy_timeout,
  dbsql_changes,
  dbsql_close,
  dbsql_collation_needed,
  dbsql_collation_needed16,
  dbsql_column_blob,
  dbsql_column_bytes,
  dbsql_column_bytes16,
  dbsql_column_count,
  dbsql_column_database_name,
  dbsql_column_database_name16,
  dbsql_column_decltype,
  dbsql_column_decltype16,
  dbsql_column_double,
  dbsql_column_int,
  dbsql_column_int64,
  dbsql_column_name,
  dbsql_column_name16,
  dbsql_column_origin_name,
  dbsql_column_origin_name16,
  dbsql_column_table_name,
  dbsql_column_table_name16,
  dbsql_column_text,
  dbsql_column_text16,
  dbsql_column_type,
  dbsql_column_value,
  dbsql_commit_hook,
  dbsql_complete,
  dbsql_complete16,
  dbsql_create_collation,
  dbsql_create_collation16,
  dbsql_create_function,
  dbsql_create_function16,
  dbsql_create_module,
  dbsql_data_count,
  dbsql_db_handle,
  dbsql_declare_vtab,
  dbsql_enable_shared_cache,
  dbsql_errcode,
  dbsql_errmsg,
  dbsql_errmsg16,
  dbsql_exec,
#ifndef DBSQL_OMIT_DEPRECATED
  dbsql_expired,
#else
  0,
#endif
  dbsql_finalize,
  dbsql_free,
  dbsql_free_table,
  dbsql_get_autocommit,
  dbsql_get_auxdata,
  dbsql_get_table,
  0,     /* Was dbsql_global_recover(), but that function is deprecated */
  dbsql_interrupt,
  dbsql_last_insert_rowid,
  dbsql_libversion,
  dbsql_libversion_number,
  dbsql_malloc,
  dbsql_mprintf,
  dbsql_open,
  dbsql_open16,
  dbsql_prepare,
  dbsql_prepare16,
  dbsql_profile,
  dbsql_progress_handler,
  dbsql_realloc,
  dbsql_reset,
  dbsql_result_blob,
  dbsql_result_double,
  dbsql_result_error,
  dbsql_result_error16,
  dbsql_result_int,
  dbsql_result_int64,
  dbsql_result_null,
  dbsql_result_text,
  dbsql_result_text16,
  dbsql_result_text16be,
  dbsql_result_text16le,
  dbsql_result_value,
  dbsql_rollback_hook,
  dbsql_set_authorizer,
  dbsql_set_auxdata,
  dbsql_snprintf,
  dbsql_step,
  dbsql_table_column_metadata,
#ifndef DBSQL_OMIT_DEPRECATED
  dbsql_thread_cleanup,
#else
  0,
#endif
  dbsql_total_changes,
  dbsql_trace,
#ifndef DBSQL_OMIT_DEPRECATED
  dbsql_transfer_bindings,
#else
  0,
#endif
  dbsql_update_hook,
  dbsql_user_data,
  dbsql_value_blob,
  dbsql_value_bytes,
  dbsql_value_bytes16,
  dbsql_value_double,
  dbsql_value_int,
  dbsql_value_int64,
  dbsql_value_numeric_type,
  dbsql_value_text,
  dbsql_value_text16,
  dbsql_value_text16be,
  dbsql_value_text16le,
  dbsql_value_type,
  dbsql_vmprintf,
  /*
  ** The original API set ends here.  All extensions can call any
  ** of the APIs above provided that the pointer is not NULL.  But
  ** before calling APIs that follow, extension should check the
  ** dbsql_libversion_number() to make sure they are dealing with
  ** a library that is new enough to support that API.
  *************************************************************************
  */
  dbsql_overload_function,

  /*
  ** Added after 3.3.13
  */
  dbsql_prepare_v2,
  dbsql_prepare16_v2,
  dbsql_clear_bindings,

  /*
  ** Added for 3.4.1
  */
  dbsql_create_module_v2,

  /*
  ** Added for 3.5.0
  */
  dbsql_bind_zeroblob,
  dbsql_blob_bytes,
  dbsql_blob_close,
  dbsql_blob_open,
  dbsql_blob_read,
  dbsql_blob_write,
  dbsql_create_collation_v2,
  dbsql_file_control,
  dbsql_memory_highwater,
  dbsql_memory_used,
#ifdef DBSQL_MUTEX_OMIT
  0, 
  0, 
  0,
  0,
  0,
#else
  dbsql_mutex_alloc,
  dbsql_mutex_enter,
  dbsql_mutex_free,
  dbsql_mutex_leave,
  dbsql_mutex_try,
#endif
  dbsql_open_v2,
  dbsql_release_memory,
  dbsql_result_error_nomem,
  dbsql_result_error_toobig,
  dbsql_sleep,
  dbsql_soft_heap_limit,
  dbsql_vfs_find,
  dbsql_vfs_register,
  dbsql_vfs_unregister,

  /*
  ** Added for 3.5.8
  */
  dbsql_threadsafe,
  dbsql_result_zeroblob,
  dbsql_result_error_code,
  dbsql_test_control,
  dbsql_randomness,
  dbsql_context_db_handle,

  /*
  ** Added for 3.6.0
  */
  dbsql_extended_result_codes,
  dbsql_limit,
  dbsql_next_stmt,
  dbsql_sql,
  dbsql_status,

  /*
  ** Added for 3.7.4
  */
  dbsql_backup_finish,
  dbsql_backup_init,
  dbsql_backup_pagecount,
  dbsql_backup_remaining,
  dbsql_backup_step,
#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
  dbsql_compileoption_get,
  dbsql_compileoption_used,
#else
  0,
  0,
#endif
  dbsql_create_function_v2,
  dbsql_db_config,
  dbsql_db_mutex,
  dbsql_db_status,
  dbsql_extended_errcode,
  dbsql_log,
  dbsql_soft_heap_limit64,
  dbsql_sourceid,
  dbsql_stmt_status,
  dbsql_strnicmp,
#ifdef DBSQL_ENABLE_UNLOCK_NOTIFY
  dbsql_unlock_notify,
#else
  0,
#endif
#ifndef DBSQL_OMIT_WAL
  dbsql_wal_autocheckpoint,
  dbsql_wal_checkpoint,
  dbsql_wal_hook,
#else
  0,
  0,
  0,
#endif
  dbsql_blob_reopen,
  dbsql_vtab_config,
  dbsql_vtab_on_conflict,
  dbsql_close_v2,
  dbsql_db_filename,
  dbsql_db_readonly,
  dbsql_db_release_memory,
  dbsql_errstr,
  dbsql_stmt_busy,
  dbsql_stmt_readonly,
  dbsql_stricmp,
  dbsql_uri_boolean,
  dbsql_uri_int64,
  dbsql_uri_parameter,
  dbsql_vsnprintf,
  dbsql_wal_checkpoint_v2,
  /* Version 3.8.7 and later */
  dbsql_auto_extension,
  dbsql_bind_blob64,
  dbsql_bind_text64,
  dbsql_cancel_auto_extension,
  dbsql_load_extension,
  dbsql_malloc64,
  dbsql_msize,
  dbsql_realloc64,
  dbsql_reset_auto_extension,
  dbsql_result_blob64,
  dbsql_result_text64,
  dbsql_strglob,
  /* Version 3.8.11 and later */
  (dbsql_value_t*(*)(const dbsql_value_t*))dbsql_value_dup,
  dbsql_value_free,
  dbsql_result_zeroblob64,
  dbsql_bind_zeroblob64,
  /* Version 3.9.0 and later */
  dbsql_value_subtype,
  dbsql_result_subtype,
  /* Version 3.10.0 and later */
  dbsql_status64,
  dbsql_strlike,
  dbsql_db_cacheflush,
  /* Version 3.12.0 and later */
  dbsql_system_errno,
  /* Version 3.14.0 and later */
  dbsql_trace_v2,
  dbsql_expanded_sql,
  /* Version 3.18.0 and later */
  dbsql_set_last_insert_rowid,
  /* Version 3.20.0 and later */
  dbsql_prepare_v3,
  dbsql_prepare16_v3,
  dbsql_bind_pointer,
  dbsql_result_pointer,
  dbsql_value_pointer,
  /* Version 3.22.0 and later */
  dbsql_vtab_nochange,
  dbsql_value_nochange,
  dbsql_vtab_collation,
  /* Version 3.24.0 and later */
  dbsql_keyword_count,
  dbsql_keyword_name,
  dbsql_keyword_check,
  dbsql_str_new,
  dbsql_str_finish,
  dbsql_str_appendf,
  dbsql_str_vappendf,
  dbsql_str_append,
  dbsql_str_appendall,
  dbsql_str_appendchar,
  dbsql_str_reset,
  dbsql_str_errcode,
  dbsql_str_length,
  dbsql_str_value,
  /* Version 3.25.0 and later */
  dbsql_create_window_function,
  /* Version 3.26.0 and later */
#ifdef DBSQL_ENABLE_NORMALIZE
  dbsql_normalized_sql,
#else
  0,
#endif
  /* Version 3.28.0 and later */
  dbsql_stmt_isexplain,
  dbsql_value_frombind,
  /* Version 3.30.0 and later */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_drop_modules,
#else
  0,
#endif
  /* Version 3.31.0 and later */
  dbsql_hard_heap_limit64,
  dbsql_uri_key,
  dbsql_filename_database,
  dbsql_filename_journal,
  dbsql_filename_wal,
  /* Version 3.32.0 and later */
  dbsql_create_filename,
  dbsql_free_filename,
  dbsql_database_file_object,
  /* Version 3.34.0 and later */
  dbsql_txn_state,
  /* Version 3.36.1 and later */
  dbsql_changes64,
  dbsql_total_changes64,
  /* Version 3.37.0 and later */
  dbsql_autovacuum_pages,
  /* Version 3.38.0 and later */
  dbsql_error_offset,
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_vtab_rhs_value,
  dbsql_vtab_distinct,
  dbsql_vtab_in,
  dbsql_vtab_in_first,
  dbsql_vtab_in_next,
#else
  0,
  0,
  0,
  0,
  0,
#endif
  /* Version 3.39.0 and later */
#ifndef DBSQL_OMIT_DESERIALIZE
  dbsql_deserialize,
  dbsql_serialize,
#else
  0,
  0,
#endif
  dbsql_db_name,
  /* Version 3.40.0 and later */
  dbsql_value_encoding,
  /* Version 3.41.0 and later */
  dbsql_is_interrupted,
  /* Version 3.43.0 and later */
  dbsql_stmt_explain,
  /* Version 3.44.0 and later */
  dbsql_get_clientdata,
  dbsql_set_clientdata,
  /* Version 3.50.0 and later */
  dbsql_setlk_timeout,
  /* Version 3.51.0 and later */
  dbsql_set_errmsg,
  dbsql_db_status64,
  /* Version 3.52.0 and later */
  dbsql_str_truncate,
  dbsql_str_free,
#ifdef DBSQL_ENABLE_CARRAY
  dbsql_carray_bind,
  dbsql_carray_bind_v2
#else
  0,
  0
#endif
};

/* True if x is the directory separator character
*/
#if DBSQL_OS_WIN
# define DIR_SEP(X)  ((X)=='/'||(X)=='\\')
#else
# define DIR_SEP(X)  ((X)=='/')
#endif

/*
** Attempt to load an dbsql extension library contained in the file
** file.  The entry point is proc.  proc may be 0 in which case a
** default entry point name (dbsql_extension_init) is used.  Use
** of the default name is recommended.
**
** Return DBSQL_OK on success and DBSQL_ERROR if something goes wrong.
**
** If an error occurs and errmsg_p is not 0, then fill *errmsg_p with 
** error message text.  The calling function should free this memory
** by calling __dbsql_db_free(db, ).
*/
static int __load_extension(
  DBSQL *db,          /* Load the extension into this database connection */
  const char *file,    /* Name of the shared library containing extension */
  const char *proc,    /* Entry point.  Use "dbsql_extension_init" if 0 */
  char **errmsg_p       /* Put error message here if not 0 */
){
  dbsql_vfs_t *vfs = db->vfs;
  void *handle;
  dbsql_loadext_entry init;
  char *errmsg = 0;
  const char *entry;
  char *alt_entry = 0;
  void **handles;
  u_int64_t n_msg = strlen(file);
  int ii;
  int rc;

  /* Shared library endings to try if file cannot be loaded as written */
  static const char *endings[] = {
#if DBSQL_OS_WIN
     "dll"   
#elif defined(__APPLE__)
     "dylib"
#else
     "so"
#endif
  };


  if( errmsg_p ) *errmsg_p = 0;

  /* Ticket #1863.  To avoid a creating security problems for older
  ** applications that relink against newer versions of dbsql, the
  ** ability to run load_extension is turned off by default.  One
  ** must call either dbsql_enable_load_extension(db) or
  ** dbsql_db_config(db, DBSQL_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, 0)
  ** to turn on extension loading.
  */
  if( (db->flags & DBSQL_LoadExtension)==0 ){
    if( errmsg_p ){
      *errmsg_p = dbsql_mprintf("not authorized");
    }
    return DBSQL_ERROR;
  }

  entry = proc ? proc : "dbsql_extension_init";

  /* tag-20210611-1.  Some dlopen() implementations will segfault if given
  ** an oversize filename.  Most filesystems have a pathname limit of 4K,
  ** so limit the extension filename length to about twice that.
  ** https://sqlite.org/forum/forumpost/08a0d6d9bf
  **
  ** Later (2023-03-25): Save an extra 6 bytes for the filename suffix.
  ** See https://sqlite.org/forum/forumpost/24083b579d.
  */
  if( n_msg>DBSQL_MAX_PATHLEN ) goto extension_not_found;

  /* Do not allow dbsql_load_extension() to link to a copy of the
  ** running application, by passing in an empty filename. */
  if( n_msg==0 ) goto extension_not_found;
    
  handle = __dbsql_os_dl_open(vfs, file);
#if DBSQL_OS_UNIX || DBSQL_OS_WIN
  for(ii=0; ii<ARRAY_SIZE(endings) && handle==0; ii++){
    char *alt_file = dbsql_mprintf("%s.%s", file, endings[ii]);
    if( alt_file==0 ) return DBSQL_NOMEM_BKPT;
    if( n_msg+strlen(endings[ii])+1<=DBSQL_MAX_PATHLEN ){
      handle = __dbsql_os_dl_open(vfs, alt_file);
    }
    dbsql_free(alt_file);
  }
#endif
  if( handle==0 ) goto extension_not_found;
  init = (dbsql_loadext_entry)__dbsql_os_dl_sym(vfs, handle, entry);

  /* If no entry point was specified and the default legacy
  ** entry point name "dbsql_extension_init" was not found, then
  ** construct an entry point name "dbsql_X_init" where the X is
  ** replaced by the lowercase value of every ASCII alphabetic 
  ** character in the filename after the last "/" up to the first ".",
  ** and skipping the first three characters if they are "lib".  
  ** Examples:
  **
  **    /usr/local/lib/lib_example5.4.3.so ==>  dbsql_example_init
  **    C:/lib/mathfuncs.dll              ==>  dbsql_mathfuncs_init
  **
  ** If that still finds no entry point, repeat a second time but this
  ** time include both alphabetic and numeric characters up to the first
  ** ".".  Example:
  **
  **    /usr/local/lib/lib_example5.4.3.so ==>  dbsql_example5_init
  */
  if( init==0 && proc==0 ){
    int i_file, i_entry, c;
    int nc_file = __strlen30(file);
    int cnt = 0;
    alt_entry = dbsql_malloc64(nc_file+30);
    if( alt_entry==0 ){
      __dbsql_os_dl_close(vfs, handle);
      return DBSQL_NOMEM_BKPT;
    }
    do{
      memcpy(alt_entry, "dbsql_", 6);
      for(i_file=nc_file-1; i_file>=0 && !DIR_SEP(file[i_file]); i_file--){}
      i_file++;
      if( dbsql_strnicmp(file+i_file, "lib", 3)==0 ) i_file += 3;
      for(i_entry=6; (c = file[i_file])!=0 && c!='.'; i_file++){
        if( __isalpha(c) || (cnt && __isdigit(c)) ){
          alt_entry[i_entry++] = (char)__upper_to_lower[(unsigned)c];
        }
      }
      memcpy(alt_entry+i_entry, "_init", 6);
      entry = alt_entry;
      init = (dbsql_loadext_entry)__dbsql_os_dl_sym(vfs, handle, entry);
    }while( init==0 && (++cnt)<2 );
  }
  if( init==0 ){
    if( errmsg_p ){
      n_msg += strlen(entry) + 300;
      *errmsg_p = errmsg = dbsql_malloc64(n_msg);
      if( errmsg ){
        assert( n_msg<0x7fffffff );  /* errmsg would be NULL if not so */
        dbsql_snprintf((int)n_msg, errmsg,
            "no entry point [%s] in shared library [%s]", entry, file);
        __dbsql_os_dl_error(vfs, n_msg-1, errmsg);
      }
    }
    __dbsql_os_dl_close(vfs, handle);
    dbsql_free(alt_entry);
    return DBSQL_ERROR;
  }
  dbsql_free(alt_entry);
  rc = init(db, &errmsg, &__apis);
  if( rc ){
    if( rc==DBSQL_OK_LOAD_PERMANENTLY ) return DBSQL_OK;
    if( errmsg_p ){
      *errmsg_p = dbsql_mprintf("error during initialization: %s", errmsg);
    }
    dbsql_free(errmsg);
    __dbsql_os_dl_close(vfs, handle);
    return DBSQL_ERROR;
  }

  /* Append the new shared library handle to the db->extensions array. */
  handles = __db_malloc_zero(db, sizeof(handle)*(db->n_extension+1));
  if( handles==0 ){
    return DBSQL_NOMEM_BKPT;
  }
  if( db->n_extension>0 ){
    memcpy(handles, db->extensions, sizeof(handle)*db->n_extension);
  }
  __dbsql_db_free(db, db->extensions);
  db->extensions = handles;

  db->extensions[db->n_extension++] = handle;
  return DBSQL_OK;

extension_not_found:
  if( errmsg_p ){
    n_msg += 300;
    *errmsg_p = errmsg = dbsql_malloc64(n_msg);
    if( errmsg ){
      assert( n_msg<0x7fffffff );  /* errmsg would be NULL if not so */
      dbsql_snprintf((int)n_msg, errmsg,
          "unable to open shared library [%.*s]", DBSQL_MAX_PATHLEN, file);
      __dbsql_os_dl_error(vfs, n_msg-1, errmsg);
    }
  }
  return DBSQL_ERROR;
}
int dbsql_load_extension(
  DBSQL *db,          /* Load the extension into this database connection */
  const char *file,    /* Name of the shared library containing extension */
  const char *proc,    /* Entry point.  Use "dbsql_extension_init" if 0 */
  char **errmsg_p       /* Put error message here if not 0 */
){
  int rc;
  dbsql_mutex_enter(db->mutex);
  rc = __load_extension(db, file, proc, errmsg_p);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Call this routine when the database connection is closing in order
** to clean up loaded extensions
*/
void __close_extensions(DBSQL *db){
  int i;
  assert( dbsql_mutex_held(db->mutex) );
  for(i=0; i<db->n_extension; i++){
    __dbsql_os_dl_close(db->vfs, db->extensions[i]);
  }
  __dbsql_db_free(db, db->extensions);
}

/*
** Enable or disable extension loading.  Extension loading is disabled by
** default so as not to open security holes in older applications.
*/
int dbsql_enable_load_extension(DBSQL *db, int onoff){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  if( onoff ){
    db->flags |= DBSQL_LoadExtension|DBSQL_LoadExtFunc;
  }else{
    db->flags &= ~(u_int64_t)(DBSQL_LoadExtension|DBSQL_LoadExtFunc);
  }
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

#endif /* !defined(DBSQL_OMIT_LOAD_EXTENSION) */

/*
** The following object holds the list of automatically loaded
** extensions.
**
** This list is shared across threads.  The DBSQL_MUTEX_STATIC_MAIN
** mutex must be held while accessing this list.
*/
typedef struct __auto_ext_list __auto_ext_list;
static DBSQL_WSD struct __auto_ext_list {
  u_int32_t n_ext;              /* Number of entries in exts[] */
  void (**exts)(void);   /* Pointers to the extension init functions */
} __autoext = { 0, 0 };

/* The "__wsd_autoext" macro will resolve to the autoextension
** state vector.  If writable static data is unsupported on the target,
** we have to locate the state vector at run-time.  In the more common
** case where writable static data is supported, __wsd_stat can refer directly
** to the "__autoext" state vector declared above.
*/
#ifdef DBSQL_OMIT_WSD
# define WSD_AUTOEXT_INIT \
  __auto_ext_list *x = &GLOBAL(__auto_ext_list,__autoext)
# define __wsd_autoext x[0]
#else
# define WSD_AUTOEXT_INIT
# define __wsd_autoext __autoext
#endif


/*
** Register a statically linked extension that is automatically
** loaded by every new database connection.
*/
int dbsql_auto_extension(
  void (*init)(void)
){
  int rc = DBSQL_OK;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( init==0 ) return DBSQL_MISUSE_BKPT;
#endif
#ifndef DBSQL_OMIT_AUTOINIT
  rc = dbsql_initialize();
  if( rc ){
    return rc;
  }else
#endif
  {
    u_int32_t i;
#if DBSQL_THREADSAFE
    dbsql_mutex_t *mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
    WSD_AUTOEXT_INIT;
    dbsql_mutex_enter(mutex);
    for(i=0; i<__wsd_autoext.n_ext; i++){
      if( __wsd_autoext.exts[i]==init ) break;
    }
    if( i==__wsd_autoext.n_ext ){
      u_int64_t n_byte = (__wsd_autoext.n_ext+1)*sizeof(__wsd_autoext.exts[0]);
      void (**news)(void);
      news = dbsql_realloc64(__wsd_autoext.exts, n_byte);
      if( news==0 ){
        rc = DBSQL_NOMEM_BKPT;
      }else{
        __wsd_autoext.exts = news;
        __wsd_autoext.exts[__wsd_autoext.n_ext] = init;
        __wsd_autoext.n_ext++;
      }
    }
    dbsql_mutex_leave(mutex);
    assert( (rc&0xff)==rc );
    return rc;
  }
}

/*
** Cancel a prior call to dbsql_auto_extension.  Remove init from the
** set of routines that is invoked for each new database connection, if it
** is currently on the list.  If init is not on the list, then this
** routine is a no-op.
**
** Return 1 if init was found on the list and removed.  Return 0 if init
** was not on the list.
*/
int dbsql_cancel_auto_extension(
  void (*init)(void)
){
#if DBSQL_THREADSAFE
  dbsql_mutex_t *mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
  int i;
  int n = 0;
  WSD_AUTOEXT_INIT;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( init==0 ) return 0;
#endif
  dbsql_mutex_enter(mutex);
  for(i=(int)__wsd_autoext.n_ext-1; i>=0; i--){
    if( __wsd_autoext.exts[i]==init ){
      __wsd_autoext.n_ext--;
      __wsd_autoext.exts[i] = __wsd_autoext.exts[__wsd_autoext.n_ext];
      n++;
      break;
    }
  }
  dbsql_mutex_leave(mutex);
  return n;
}

/*
** Reset the automatic extension loading mechanism.
*/
void dbsql_reset_auto_extension(void){
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize()==DBSQL_OK )
#endif
  {
#if DBSQL_THREADSAFE
    dbsql_mutex_t *mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
    WSD_AUTOEXT_INIT;
    dbsql_mutex_enter(mutex);
    dbsql_free(__wsd_autoext.exts);
    __wsd_autoext.exts = 0;
    __wsd_autoext.n_ext = 0;
    dbsql_mutex_leave(mutex);
  }
}

/*
** Load all automatic extensions.
**
** If anything goes wrong, set an error in the database connection.
*/
void __auto_load_extensions(DBSQL *db){
  u_int32_t i;
  int go = 1;
  int rc;
  dbsql_loadext_entry init;

  WSD_AUTOEXT_INIT;
  if( __wsd_autoext.n_ext==0 ){
    /* Common case: early out without every having to acquire a mutex */
    return;
  }
  for(i=0; go; i++){
    char *errmsg;
#if DBSQL_THREADSAFE
    dbsql_mutex_t *mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
#ifdef DBSQL_OMIT_LOAD_EXTENSION
    const dbsql_api_routines_t *thunk = 0;
#else
    const dbsql_api_routines_t *thunk = &__apis;
#endif
    dbsql_mutex_enter(mutex);
    if( i>=__wsd_autoext.n_ext ){
      init = 0;
      go = 0;
    }else{
      init = (dbsql_loadext_entry)__wsd_autoext.exts[i];
    }
    dbsql_mutex_leave(mutex);
    errmsg = 0;
    if( init && (rc = init(db, &errmsg, thunk))!=0 ){
      __error_with_msg(db, rc,
            "automatic extension loading failed: %s", errmsg);
      go = 0;
    }
    dbsql_free(errmsg);
  }
}
