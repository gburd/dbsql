/*
** 2008 Jan 22
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
** This file contains code that modified the OS layer in order to simulate
** different device types (by overriding the return values of the 
** device_characteristics() and sector_size() methods).
*/
#if DBSQL_TEST          /* This file is used for testing only */

#include "dbsql.h"
#include "dbsql_int.h"

/*
** Maximum pathname length supported by the devsym backend.
*/
#define DEVSYM_MAX_PATHNAME 512

/*
** Name used to identify this VFS.
*/
#define DEVSYM_VFS_NAME "devsym"
#define WRITECRASH_NAME "writecrash"

typedef struct devsym_file devsym_file;
struct devsym_file {
  dbsql_file_t base;
  dbsql_file_t *real;
};

/*
** Method declarations for devsym_file.
*/
static int __devsym_close(dbsql_file_t*);
static int __devsym_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __devsym_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __devsym_truncate(dbsql_file_t*, int64_t size);
static int __devsym_sync(dbsql_file_t*, int flags);
static int __devsym_file_size(dbsql_file_t*, int64_t *size);
static int __devsym_lock(dbsql_file_t*, int);
static int __devsym_unlock(dbsql_file_t*, int);
static int __devsym_check_reserved_lock(dbsql_file_t*, int *);
static int __devsym_file_control(dbsql_file_t*, int op, void *arg);
static int __devsym_sector_size(dbsql_file_t*);
static int __devsym_device_characteristics(dbsql_file_t*);
static int __devsym_shm_lock(dbsql_file_t*,int,int,int);
static int __devsym_shm_map(dbsql_file_t*,int,int,int, void volatile **);
static void __devsym_shm_barrier(dbsql_file_t*);
static int __devsym_shm_unmap(dbsql_file_t*,int);

/*
** Method declarations for devsym_vfs.
*/
static int __devsym_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __devsym_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __devsym_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __devsym_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
#ifndef DBSQL_OMIT_LOAD_EXTENSION
static void *__devsym_dl_open(dbsql_vfs_t*, const char *filename);
static void __devsym_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*devsym_dl_sym(dbsql_vfs_t*,void*, const char *symbol))(void);
static void __devsym_dl_close(dbsql_vfs_t*, void*);
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
static int __devsym_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __devsym_sleep(dbsql_vfs_t*, int microseconds);
static int __devsym_current_time(dbsql_vfs_t*, double*);

struct devsym_global_t {
  dbsql_vfs_t *vfs;
  int i_device_char;
  int i_sector_size;
  int n_write_crash;
};
struct devsym_global_t g = {0, 0, 512, 0};

/*
** Close an devsym-file.
*/
static int __devsym_close(dbsql_file_t *file){
  devsym_file *p = (devsym_file *)file;
  __dbsql_os_close(p->real);
  return DBSQL_OK;
}

/*
** Read data from an devsym-file.
*/
static int __devsym_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_read(p->real, buf, i_amt, i_ofst);
}

/*
** Write data to an devsym-file.
*/
static int __devsym_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_write(p->real, buf, i_amt, i_ofst);
}

/*
** Truncate an devsym-file.
*/
static int __devsym_truncate(dbsql_file_t *file, int64_t size){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_truncate(p->real, size);
}

/*
** Sync an devsym-file.
*/
static int __devsym_sync(dbsql_file_t *file, int flags){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_sync(p->real, flags);
}

/*
** Return the current file-size of an devsym-file.
*/
static int __devsym_file_size(dbsql_file_t *file, int64_t *size){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_file_size(p->real, size);
}

/*
** Lock an devsym-file.
*/
static int __devsym_lock(dbsql_file_t *file, int lock){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_lock(p->real, lock);
}

/*
** Unlock an devsym-file.
*/
static int __devsym_unlock(dbsql_file_t *file, int lock){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_unlock(p->real, lock);
}

/*
** Check if another file-handle holds a RESERVED lock on an devsym-file.
*/
static int __devsym_check_reserved_lock(dbsql_file_t *file, int *res_out){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_check_reserved_lock(p->real, res_out);
}

/*
** File control method. For custom operations on an devsym-file.
*/
static int __devsym_file_control(dbsql_file_t *file, int op, void *arg){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_file_control(p->real, op, arg);
}

/*
** Return the sector-size in bytes for an devsym-file.
*/
static int __devsym_sector_size(dbsql_file_t *file){
  return g.i_sector_size;
}

/*
** Return the device characteristic flags supported by an devsym-file.
*/
static int __devsym_device_characteristics(dbsql_file_t *file){
  return g.i_device_char;
}

/*
** Shared-memory methods are all pass-throughs.
*/
static int __devsym_shm_lock(dbsql_file_t *file, int ofst, int n, int flags){
  devsym_file *p = (devsym_file *)file;
  return p->real->methods->shm_lock(p->real, ofst, n, flags);
}
static int __devsym_shm_map(
  dbsql_file_t *file, 
  int i_region, 
  int sz_region, 
  int is_write, 
  void volatile **pp
){
  devsym_file *p = (devsym_file *)file;
  return p->real->methods->shm_map(p->real, i_region, sz_region, is_write, pp);
}
static void __devsym_shm_barrier(dbsql_file_t *file){
  devsym_file *p = (devsym_file *)file;
  p->real->methods->shm_barrier(p->real);
}
static int __devsym_shm_unmap(dbsql_file_t *file, int del_flag){
  devsym_file *p = (devsym_file *)file;
  return p->real->methods->shm_unmap(p->real, del_flag);
}



/*
** Open an devsym file handle.
*/
static int __devsym_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
static dbsql_io_methods_t devsym_io_methods = {
  2,                                /* i_version */
  __devsym_close,                      /* x_close */
  __devsym_read,                       /* x_read */
  __devsym_write,                      /* x_write */
  __devsym_truncate,                   /* truncate */
  __devsym_sync,                       /* x_sync */
  __devsym_file_size,                   /* file_size */
  __devsym_lock,                       /* lock */
  __devsym_unlock,                     /* unlock */
  __devsym_check_reserved_lock,          /* check_reserved_lock */
  __devsym_file_control,                /* file_control */
  __devsym_sector_size,                 /* sector_size */
  __devsym_device_characteristics,      /* device_characteristics */
  __devsym_shm_map,                     /* shm_map */
  __devsym_shm_lock,                    /* shm_lock */
  __devsym_shm_barrier,                 /* shm_barrier */
  __devsym_shm_unmap                    /* shm_unmap */
};

  int rc;
  devsym_file *p = (devsym_file *)file;
  p->real = (dbsql_file_t *)&p[1];
  rc = __dbsql_os_open(g.vfs, name, p->real, flags, out_flags);
  if( p->real->methods ){
    file->methods = &devsym_io_methods;
  }
  return rc;
}

/*
** Delete the file located at path. If the dir_sync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int __devsym_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  return __dbsql_os_delete(g.vfs, path, dir_sync);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int __devsym_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  return __dbsql_os_access(g.vfs, path, flags, res_out);
}

/*
** Populate buffer out with the full canonical pathname corresponding
** to the pathname in path. out is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int __devsym_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  return __dbsql_os_full_pathname(g.vfs, path, n_out, out);
}

#ifndef DBSQL_OMIT_LOAD_EXTENSION
/*
** Open the dynamic library located at path and return a handle.
*/
static void *__devsym_dl_open(dbsql_vfs_t *vfs, const char *path){
  return __dbsql_os_dl_open(g.vfs, path);
}

/*
** Populate the buffer errmsg (size n_byte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void __devsym_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  __dbsql_os_dl_error(g.vfs, n_byte, errmsg);
}

/*
** Return a pointer to the symbol symbol in the dynamic library handle.
*/
static void (*devsym_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return __dbsql_os_dl_sym(g.vfs, p, sym);
}

/*
** Close the dynamic library handle handle.
*/
static void __devsym_dl_close(dbsql_vfs_t *vfs, void *handle){
  __dbsql_os_dl_close(g.vfs, handle);
}
#endif /* DBSQL_OMIT_LOAD_EXTENSION */

/*
** Populate the buffer pointed to by buf_out with n_byte bytes of 
** random data.
*/
static int __devsym_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return __dbsql_os_randomness(g.vfs, n_byte, buf_out);
}

/*
** Sleep for n_micro microseconds. Return the number of microseconds 
** actually slept.
*/
static int __devsym_sleep(dbsql_vfs_t *vfs, int n_micro){
  return __dbsql_os_sleep(g.vfs, n_micro);
}

/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __devsym_current_time(dbsql_vfs_t *vfs, double *time_out){
  return g.vfs->current_time(g.vfs, time_out);
}

/*
** Return the sector-size in bytes for an writecrash-file.
*/
static int __writecrash_sector_size(dbsql_file_t *file){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_sector_size(p->real);
}

/*
** Return the device characteristic flags supported by an writecrash-file.
*/
static int __writecrash_device_characteristics(dbsql_file_t *file){
  devsym_file *p = (devsym_file *)file;
  return __dbsql_os_device_characteristics(p->real);
}

/*
** Write data to an writecrash-file.
*/
static int __writecrash_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  devsym_file *p = (devsym_file *)file;
  if( g.n_write_crash>0 ){
    g.n_write_crash--;
    if( g.n_write_crash==0 ) abort();
  }
  return __dbsql_os_write(p->real, buf, i_amt, i_ofst);
}

/*
** Open an writecrash file handle.
*/
static int __writecrash_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
static dbsql_io_methods_t writecrash_io_methods = {
  2,                                /* i_version */
  __devsym_close,                      /* x_close */
  __devsym_read,                       /* x_read */
  __writecrash_write,                  /* x_write */
  __devsym_truncate,                   /* truncate */
  __devsym_sync,                       /* x_sync */
  __devsym_file_size,                   /* file_size */
  __devsym_lock,                       /* lock */
  __devsym_unlock,                     /* unlock */
  __devsym_check_reserved_lock,          /* check_reserved_lock */
  __devsym_file_control,                /* file_control */
  __writecrash_sector_size,             /* sector_size */
  __writecrash_device_characteristics,  /* device_characteristics */
  __devsym_shm_map,                     /* shm_map */
  __devsym_shm_lock,                    /* shm_lock */
  __devsym_shm_barrier,                 /* shm_barrier */
  __devsym_shm_unmap                    /* shm_unmap */
};

  int rc;
  devsym_file *p = (devsym_file *)file;
  p->real = (dbsql_file_t *)&p[1];
  rc = __dbsql_os_open(g.vfs, name, p->real, flags, out_flags);
  if( p->real->methods ){
    file->methods = &writecrash_io_methods;
  }
  return rc;
}

static dbsql_vfs_t devsym_vfs = {
  2,                     /* i_version */
  sizeof(devsym_file),      /* sz_os_file */
  DEVSYM_MAX_PATHNAME,      /* mx_pathname */
  0,                     /* next */
  DEVSYM_VFS_NAME,          /* name */
  0,                     /* app_data */
  __devsym_open,               /* x_open */
  __devsym_delete,             /* x_delete */
  __devsym_access,             /* x_access */
  __devsym_full_pathname,       /* full_pathname */
#ifndef DBSQL_OMIT_LOAD_EXTENSION
  __devsym_dl_open,             /* dl_open */
  __devsym_dl_error,            /* dl_error */
  devsym_dl_sym,              /* dl_sym */
  __devsym_dl_close,            /* dl_close */
#else
  0,                        /* dl_open */
  0,                        /* dl_error */
  0,                        /* dl_sym */
  0,                        /* dl_close */
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
  __devsym_randomness,         /* randomness */
  __devsym_sleep,              /* x_sleep */
  __devsym_current_time,        /* current_time */
  0,                        /* get_last_error */
  0                         /* current_time_int64 */
};

static dbsql_vfs_t writecrash_vfs = {
  2,                     /* i_version */
  sizeof(devsym_file),      /* sz_os_file */
  DEVSYM_MAX_PATHNAME,      /* mx_pathname */
  0,                     /* next */
  WRITECRASH_NAME,          /* name */
  0,                     /* app_data */
  __writecrash_open,           /* x_open */
  __devsym_delete,             /* x_delete */
  __devsym_access,             /* x_access */
  __devsym_full_pathname,       /* full_pathname */
#ifndef DBSQL_OMIT_LOAD_EXTENSION
  __devsym_dl_open,             /* dl_open */
  __devsym_dl_error,            /* dl_error */
  devsym_dl_sym,              /* dl_sym */
  __devsym_dl_close,            /* dl_close */
#else
  0,                        /* dl_open */
  0,                        /* dl_error */
  0,                        /* dl_sym */
  0,                        /* dl_close */
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
  __devsym_randomness,         /* randomness */
  __devsym_sleep,              /* x_sleep */
  __devsym_current_time,        /* current_time */
  0,                        /* get_last_error */
  0                         /* current_time_int64 */
};


/*
** This procedure registers the devsym vfs with dbsql. If the argument is
** true, the devsym vfs becomes the new default vfs. It is the only publicly
** available function in this file.
*/
void devsym_register(int i_device_char, int i_sector_size){

  if( g.vfs==0 ){
    g.vfs = dbsql_vfs_find(0);
    devsym_vfs.sz_os_file += g.vfs->sz_os_file;
    writecrash_vfs.sz_os_file += g.vfs->sz_os_file;
    dbsql_vfs_register(&devsym_vfs, 0);
    dbsql_vfs_register(&writecrash_vfs, 0);
  }
  if( i_device_char>=0 ){
    g.i_device_char = i_device_char;
  }else{
    g.i_device_char = 0;
  }
  if( i_sector_size>=0 ){
    g.i_sector_size = i_sector_size;
  }else{
    g.i_sector_size = 512;
  }
}

void devsym_unregister(){
  dbsql_vfs_unregister(&devsym_vfs);
  dbsql_vfs_unregister(&writecrash_vfs);
  g.vfs = 0;
  g.i_device_char = 0;
  g.i_sector_size = 0;
}

void devsym_crash_on_write(int n_write){
  if( g.vfs==0 ){
    g.vfs = dbsql_vfs_find(0);
    devsym_vfs.sz_os_file += g.vfs->sz_os_file;
    writecrash_vfs.sz_os_file += g.vfs->sz_os_file;
    dbsql_vfs_register(&devsym_vfs, 0);
    dbsql_vfs_register(&writecrash_vfs, 0);
  }
  g.n_write_crash = n_write;
}

#endif
