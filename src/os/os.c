/*
** 2005 November 29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains OS interface code that is common to all
** architectures.
*/
#include "dbsql_int.h"

/*
** If we compile with the DBSQL_TEST macro set, then the following block
** of code will give us the ability to simulate a disk I/O error.  This
** is used for testing the I/O recovery logic.
*/
#if defined(DBSQL_TEST)
int dbsql_io_error_hit = 0;            /* Total number of I/O Errors */
int dbsql_io_error_hardhit = 0;        /* Number of non-benign errors */
int dbsql_io_error_pending = 0;        /* Count down to first I/O error */
int dbsql_io_error_persist = 0;        /* True if I/O errors persist */
int dbsql_io_error_benign = 0;         /* True if errors are benign */
int dbsql_diskfull_pending = 0;
int dbsql_diskfull = 0;
#endif /* defined(DBSQL_TEST) */

/*
** When testing, also keep a count of the number of open files.
*/
#if defined(DBSQL_TEST)
int dbsql_open_file_count = 0;
#endif /* defined(DBSQL_TEST) */

/*
** The default dbsql dbsql_vfs_t implementations do not allocate
** memory (actually, os_unix.c allocates a small amount of memory
** from within __os_open()), but some third-party implementations may.
** So we test the effects of a malloc() failing and the __dbsql_os_xxx()
** function returning DBSQL_IOERR_NOMEM using the DO_OS_MALLOC_TEST macro.
**
** The following functions are instrumented for malloc() failure
** testing:
**
**     __dbsql_os_read()
**     __dbsql_os_write()
**     __dbsql_os_sync()
**     __dbsql_os_file_size()
**     __dbsql_os_lock()
**     __dbsql_os_check_reserved_lock()
**     __dbsql_os_file_control()
**     __dbsql_os_shm_map()
**     __dbsql_os_open()
**     __dbsql_os_delete()
**     __dbsql_os_access()
**     __dbsql_os_full_pathname()
**
*/
#if defined(DBSQL_TEST)
int dbsql_memdebug_vfs_oom_test = 1;
  #define DO_OS_MALLOC_TEST(x)                                       \
  if (dbsql_memdebug_vfs_oom_test && (!x || !__journal_is_in_memory(x))) { \
    void *tst_alloc = __dbsql_malloc(10);                             \
    if (!tst_alloc) return DBSQL_IOERR_NOMEM_BKPT;                  \
    dbsql_free(tst_alloc);                                         \
  }
#else
  #define DO_OS_MALLOC_TEST(x)
#endif

/*
** The following routines are convenience wrappers around methods
** of the dbsql_file_t object.  This is mostly just syntactic sugar. All
** of this would be completely automatic if dbsql were coded using
** C++ instead of plain old C.
*/
void __dbsql_os_close(dbsql_file_t *id){
  if( id->methods ){
    id->methods->x_close(id);
    id->methods = 0;
  }
}
int __dbsql_os_read(dbsql_file_t *id, void *buf, int amt, int64_t offset){
  DO_OS_MALLOC_TEST(id);
  return id->methods->x_read(id, buf, amt, offset);
}
int __dbsql_os_write(dbsql_file_t *id, const void *buf, int amt, int64_t offset){
  DO_OS_MALLOC_TEST(id);
  return id->methods->x_write(id, buf, amt, offset);
}
int __dbsql_os_truncate(dbsql_file_t *id, int64_t size){
  return id->methods->truncate(id, size);
}
int __dbsql_os_sync(dbsql_file_t *id, int flags){
  DO_OS_MALLOC_TEST(id);
  return flags ? id->methods->x_sync(id, flags) : DBSQL_OK;
}
int __dbsql_os_file_size(dbsql_file_t *id, int64_t *size){
  DO_OS_MALLOC_TEST(id);
  return id->methods->file_size(id, size);
}
int __dbsql_os_lock(dbsql_file_t *id, int lock_type){
  DO_OS_MALLOC_TEST(id);
  assert( lock_type>=DBSQL_LOCK_SHARED && lock_type<=DBSQL_LOCK_EXCLUSIVE );
  return id->methods->lock(id, lock_type);
}
int __dbsql_os_unlock(dbsql_file_t *id, int lock_type){
  assert( lock_type==DBSQL_LOCK_NONE || lock_type==DBSQL_LOCK_SHARED );
  return id->methods->unlock(id, lock_type);
}
int __dbsql_os_check_reserved_lock(dbsql_file_t *id, int *res_out){
  DO_OS_MALLOC_TEST(id);
  return id->methods->check_reserved_lock(id, res_out);
}

/*
** Use __dbsql_os_file_control() when we are doing something that might fail
** and we need to know about the failures.  Use __dbsql_os_file_control_hint()
** when simply tossing information over the wall to the VFS and we do not
** really care if the VFS receives and understands the information since it
** is only a hint and can be safely ignored.  The __dbsql_os_file_control_hint()
** routine has no return value since the return value would be meaningless.
*/
int __dbsql_os_file_control(dbsql_file_t *id, int op, void *arg){
  if( id->methods==0 ) return DBSQL_NOTFOUND;
#ifdef DBSQL_TEST
  if( op!=DBSQL_FCNTL_COMMIT_PHASETWO
   && op!=DBSQL_FCNTL_LOCK_TIMEOUT
   && op!=DBSQL_FCNTL_CKPT_DONE
   && op!=DBSQL_FCNTL_CKPT_START
  ){
    /* Faults are not injected into COMMIT_PHASETWO because, assuming dbsql
    ** is using a regular VFS, it is called after the corresponding
    ** transaction has been committed. Injecting a fault at this point
    ** confuses the test scripts - the COMMIT command returns DBSQL_NOMEM
    ** but the transaction is committed anyway.
    **
    ** The core must call __os_file_control() though, not __os_file_control_hint(),
    ** as if a custom VFS (e.g. zipvfs) returns an error here, it probably
    ** means the commit really has failed and an error should be returned
    ** to the user.
    **
    ** The CKPT_DONE and CKPT_START file-controls are write-only signals
    ** to the cksumvfs.  Their return code is meaningless and is ignored
    ** by the dbsql core, so there is no point in simulating OOMs for them.
    */
    DO_OS_MALLOC_TEST(id);
  }
#endif
  return id->methods->file_control(id, op, arg);
}
void __dbsql_os_file_control_hint(dbsql_file_t *id, int op, void *arg){
  if( id->methods ) (void)id->methods->file_control(id, op, arg);
}

int __dbsql_os_sector_size(dbsql_file_t *id){
  int (*sector_size)(dbsql_file_t*) = id->methods->sector_size;
  return (sector_size ? sector_size(id) : DBSQL_DEFAULT_SECTOR_SIZE);
}
int __dbsql_os_device_characteristics(dbsql_file_t *id){
  if( NEVER(id->methods==0) ) return 0;
  return id->methods->device_characteristics(id);
}
#ifndef DBSQL_OMIT_WAL
int __dbsql_os_shm_lock(dbsql_file_t *id, int offset, int n, int flags){
  return id->methods->shm_lock(id, offset, n, flags);
}
void __dbsql_os_shm_barrier(dbsql_file_t *id){
  id->methods->shm_barrier(id);
}
int __dbsql_os_shm_unmap(dbsql_file_t *id, int delete_flag){
  return id->methods->shm_unmap(id, delete_flag);
}
int __dbsql_os_shm_map(
  dbsql_file_t *id,               /* Database file handle */
  int i_page,
  int pgsz,
  int extend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Pointer to mapping */
){
  DO_OS_MALLOC_TEST(id);
  return id->methods->shm_map(id, i_page, pgsz, extend, pp);
}
#endif /* DBSQL_OMIT_WAL */

#if DBSQL_MAX_MMAP_SIZE>0
/* The real implementation of fetch and unfetch */
int __dbsql_os_fetch(dbsql_file_t *id, int64_t i_off, int i_amt, void **pp){
  DO_OS_MALLOC_TEST(id);
  return id->methods->fetch(id, i_off, i_amt, pp);
}
int __dbsql_os_unfetch(dbsql_file_t *id, int64_t i_off, void *p){
  return id->methods->unfetch(id, i_off, p);
}
#else
/* No-op stubs to use when memory-mapped I/O is disabled */
int __dbsql_os_fetch(dbsql_file_t *id, int64_t i_off, int i_amt, void **pp){
  *pp = 0;
  return DBSQL_OK;
}
int __dbsql_os_unfetch(dbsql_file_t *id, int64_t i_off, void *p){
  return DBSQL_OK;
}
#endif

/*
** The next group of routines are convenience wrappers around the
** VFS methods.
*/
int __dbsql_os_open(
  dbsql_vfs_t *vfs,
  const char *path,
  dbsql_file_t *file,
  int flags,
  int *flags_out
){
  int rc;
  DO_OS_MALLOC_TEST(0);
  /* 0x87f7f is a mask of DBSQL_OPEN_ flags that are valid to be passed
  ** down into the VFS layer.  Some DBSQL_OPEN_ flags (for example,
  ** DBSQL_OPEN_FULLMUTEX or DBSQL_OPEN_SHAREDCACHE) are blocked before
  ** reaching the VFS. */
  assert( path || (flags & DBSQL_OPEN_EXCLUSIVE) );
  rc = vfs->x_open(vfs, path, file, flags & 0x1087f7f, flags_out);
  assert( rc==DBSQL_OK || file->methods==0 );
  return rc;
}
int __dbsql_os_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  DO_OS_MALLOC_TEST(0);
  assert( dir_sync==0 || dir_sync==1 );
  return vfs->x_delete!=0 ? vfs->x_delete(vfs, path, dir_sync) : DBSQL_OK;
}
int __dbsql_os_access(
  dbsql_vfs_t *vfs,
  const char *path,
  int flags,
  int *res_out
){
  DO_OS_MALLOC_TEST(0);
  return vfs->x_access(vfs, path, flags, res_out);
}
int __dbsql_os_full_pathname(
  dbsql_vfs_t *vfs,
  const char *path,
  int n_path_out,
  char *path_out
){
  DO_OS_MALLOC_TEST(0);
  path_out[0] = 0;
  return vfs->full_pathname(vfs, path, n_path_out, path_out);
}
#ifndef DBSQL_OMIT_LOAD_EXTENSION
void *__dbsql_os_dl_open(dbsql_vfs_t *vfs, const char *path){
  assert( path!=0 );
  assert( strlen(path)<=DBSQL_MAX_PATHLEN );  /* tag-20210611-1 */
  return vfs->dl_open(vfs, path);
}
void __dbsql_os_dl_error(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  vfs->dl_error(vfs, n_byte, buf_out);
}
void (*__dbsql_os_dl_sym(dbsql_vfs_t *vfs, void *hdle, const char *sym))(void){
  return vfs->dl_sym(vfs, hdle, sym);
}
void __dbsql_os_dl_close(dbsql_vfs_t *vfs, void *handle){
  vfs->dl_close(vfs, handle);
}
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
int __dbsql_os_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  if( __config.i_prng_seed ){
    memset(buf_out, 0, n_byte);
    if( ALWAYS(n_byte>(signed)sizeof(unsigned)) ) n_byte = sizeof(unsigned int);
    memcpy(buf_out, &__config.i_prng_seed, n_byte);
    return DBSQL_OK;
  }else{
    return vfs->randomness(vfs, n_byte, buf_out);
  }

}
int __dbsql_os_sleep(dbsql_vfs_t *vfs, int n_micro){
  return vfs->x_sleep(vfs, n_micro);
}
int __dbsql_os_get_last_error(dbsql_vfs_t *vfs){
  return vfs->get_last_error ? vfs->get_last_error(vfs, 0, 0) : 0;
}
int __dbsql_os_current_time_int64(dbsql_vfs_t *vfs, int64_t *time_out){
  int rc;
  /* IMPLEMENTATION-OF: R-49045-42493 dbsql will use the current_time_int64()
  ** method to get the current date and time if that method is available
  ** (if i_version is 2 or greater and the function pointer is not NULL) and
  ** will fall back to current_time() if current_time_int64() is
  ** unavailable.
  */
  if( vfs->i_version>=2 && vfs->current_time_int64 ){
    rc = vfs->current_time_int64(vfs, time_out);
  }else{
    double r;
    rc = vfs->current_time(vfs, &r);
    *time_out = __real_to_i64(r*86400000.0);
  }
  return rc;
}

int __dbsql_os_open_malloc(
  dbsql_vfs_t *vfs,
  const char *z_file,
  dbsql_file_t **pp_file,
  int flags,
  int *out_flags
){
  int rc;
  dbsql_file_t *file;
  file = (dbsql_file_t *)__malloc_zero(vfs->sz_os_file);
  if( file ){
    rc = __dbsql_os_open(vfs, z_file, file, flags, out_flags);
    if( rc!=DBSQL_OK ){
      dbsql_free(file);
      *pp_file = 0;
    }else{
      *pp_file = file;
    }
  }else{
    *pp_file = 0;
    rc = DBSQL_NOMEM_BKPT;
  }
  assert( *pp_file!=0 || rc!=DBSQL_OK );
  return rc;
}
void __dbsql_os_close_free(dbsql_file_t *file){
  assert( file );
  __dbsql_os_close(file);
  dbsql_free(file);
}

/*
** This function is a wrapper around the OS specific implementation of
** dbsql_os_init(). The purpose of the wrapper is to provide the
** ability to simulate a malloc failure, so that the handling of an
** error in dbsql_os_init() by the upper layers can be tested.
*/
int __dbsql_os_init(void){
  void *p = dbsql_malloc(10);
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  dbsql_free(p);
  return dbsql_os_init();
}

/*
** The list of all registered VFS implementations.
*/
static dbsql_vfs_t * DBSQL_WSD VFS_LIST = 0;
#define VFS_LIST GLOBAL(dbsql_vfs_t *, VFS_LIST)

/*
** Locate a VFS by name.  If no name is given, simply return the
** first VFS on the list.
*/
dbsql_vfs_t *dbsql_vfs_find(const char *z_vfs){
  dbsql_vfs_t *vfs = 0;
#if DBSQL_THREADSAFE
  dbsql_mutex_t *mutex;
#endif
#ifndef DBSQL_OMIT_AUTOINIT
  int rc = dbsql_initialize();
  if( rc ) return 0;
#endif
#if DBSQL_THREADSAFE
  mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
  dbsql_mutex_enter(mutex);
  for(vfs = VFS_LIST; vfs; vfs=vfs->next){
    if( z_vfs==0 ) break;
    if( strcmp(z_vfs, vfs->name)==0 ) break;
  }
  dbsql_mutex_leave(mutex);
  return vfs;
}

/*
** Unlink a VFS from the linked list
*/
static void __vfs_unlink(dbsql_vfs_t *vfs){
  assert( dbsql_mutex_held(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN)) );
  if( vfs==0 ){
    /* No-op */
  }else if( VFS_LIST==vfs ){
    VFS_LIST = vfs->next;
  }else if( VFS_LIST ){
    dbsql_vfs_t *p = VFS_LIST;
    while( p->next && p->next!=vfs ){
      p = p->next;
    }
    if( p->next==vfs ){
      p->next = vfs->next;
    }
  }
}

/*
** Register a VFS with the system.  It is harmless to register the same
** VFS multiple times.  The new VFS becomes the default if make_dflt is
** true.
*/
int dbsql_vfs_register(dbsql_vfs_t *vfs, int make_dflt){
  MUTEX_LOGIC(dbsql_mutex_t *mutex;)
#ifndef DBSQL_OMIT_AUTOINIT
  int rc = dbsql_initialize();
  if( rc ) return rc;
#endif
#ifdef DBSQL_ENABLE_API_ARMOR
  if( vfs==0 ) return DBSQL_MISUSE_BKPT;
#endif

  MUTEX_LOGIC( mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN); )
  dbsql_mutex_enter(mutex);
  __vfs_unlink(vfs);
  if( make_dflt || VFS_LIST==0 ){
    vfs->next = VFS_LIST;
    VFS_LIST = vfs;
  }else{
    vfs->next = VFS_LIST->next;
    VFS_LIST->next = vfs;
  }
  assert(VFS_LIST);
  dbsql_mutex_leave(mutex);
  return DBSQL_OK;
}

/*
** Unregister a VFS so that it is no longer accessible.
*/
int dbsql_vfs_unregister(dbsql_vfs_t *vfs){
  MUTEX_LOGIC(dbsql_mutex_t *mutex;)
#ifndef DBSQL_OMIT_AUTOINIT
  int rc = dbsql_initialize();
  if( rc ) return rc;
#endif
  MUTEX_LOGIC( mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN); )
  dbsql_mutex_enter(mutex);
  __vfs_unlink(vfs);
  dbsql_mutex_leave(mutex);
  return DBSQL_OK;
}
