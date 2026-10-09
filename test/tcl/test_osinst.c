/*
** 2008 April 10
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
** This file contains the implementation of an dbsql vfs wrapper that
** adds instrumentation to all vfs and file methods. C and Tcl interfaces
** are provided to control the instrumentation.
*/

/*
** This module contains code for a wrapper VFS that causes a log of
** most VFS calls to be written into a nominated file on disk. The log 
** is stored in a compressed binary format to reduce the amount of IO 
** overhead introduced into the application by logging.
**
** All calls on dbsql_file_t objects except file_control() are logged.
** Additionally, calls to the x_access(), x_open(), and x_delete()
** methods are logged. The other dbsql_vfs_t object methods (dl_xxx,
** randomness, x_sleep, current_time, get_last_error and current_time_int64) 
** are not logged.
**
** The binary log files are read using a virtual table implementation
** also contained in this file. 
**
** CREATING LOG FILES:
**
**       int dbsql_vfslog_new(
**         const char *vfs,          // Name of new VFS
**         const char *parent_vfs,    // Name of parent VFS (or NULL)
**         const char *z_log           // Name of log file to write to
**       );
**
**       int dbsql_vfslog_finalize(const char *vfs);
**
** ANNOTATING LOG FILES:
**
**   To write an arbitrary message into a log file:
**
**       int dbsql_vfslog_annotate(const char *vfs, const char *msg);
**
** READING LOG FILES:
**
**   Log files are read using the "vfslog" virtual table implementation
**   in this file. To register the virtual table with dbsql, use:
**
**       int dbsql_vfslog_register(DBSQL *db);
**
**   Then, if the log file is named "vfs.log", the following SQL command:
**
**       CREATE VIRTUAL TABLE v USING vfslog('vfs.log');
**
**   creates a virtual table with 6 columns, as follows:
**
**       CREATE TABLE v(
**         event    TEXT,             // "x_open", "x_read" etc.
**         file     TEXT,             // Name of file this call applies to
**         clicks   INTEGER,          // Time spent in call
**         rc       INTEGER,          // Return value
**         size     INTEGER,          // Bytes read or written
**         offset   INTEGER           // File offset read or written
**       );
*/

#include "dbsql.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <string.h>
#include <assert.h>


/*
** Maximum pathname length supported by the vfslog backend.
*/
#define INST_MAX_PATHNAME 512

#define OS_ACCESS            1
#define OS_CHECKRESERVEDLOCK 2
#define OS_CLOSE             3
#define OS_CURRENTTIME       4
#define OS_DELETE            5
#define OS_DEVCHAR           6
#define OS_FILECONTROL       7
#define OS_FILESIZE          8
#define OS_FULLPATHNAME      9
#define OS_LOCK              11
#define OS_OPEN              12
#define OS_RANDOMNESS        13
#define OS_READ              14 
#define OS_SECTORSIZE        15
#define OS_SLEEP             16
#define OS_SYNC              17
#define OS_TRUNCATE          18
#define OS_UNLOCK            19
#define OS_WRITE             20
#define OS_SHMUNMAP          22
#define OS_SHMMAP            23
#define OS_SHMLOCK           25
#define OS_SHMBARRIER        26
#define OS_ANNOTATE          28

#define OS_NUMEVENTS         29

#define VFSLOG_BUFFERSIZE 8192

typedef struct vfslog_vfs_t vfslog_vfs_t;
typedef struct vfslog_file_t vfslog_file_t;

struct vfslog_vfs_t {
  dbsql_vfs_t base;               /* VFS methods */
  dbsql_vfs_t *vfs;              /* Parent VFS */
  int i_next_file_id;                /* Next file id */
  dbsql_file_t *p_log;             /* Log file handle */
  int64_t i_offset;          /* Log file offset of start of write buffer */
  int n_buf;                       /* Number of valid bytes in bufs[] */
  char bufs[VFSLOG_BUFFERSIZE];   /* Write buffer */
};

struct vfslog_file_t {
  dbsql_file_t base;              /* IO methods */
  dbsql_file_t *real;            /* Underlying file handle */
  dbsql_vfs_t *vfslog;           /* Associated __vsflog_vfs object */
  int i_file_id;                    /* File id number */
};

#define REALVFS(p) (((vfslog_vfs_t *)(p))->vfs)



/*
** Method declarations for vfslog_file.
*/
static int __vfslog_close(dbsql_file_t*);
static int __vfslog_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __vfslog_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __vfslog_truncate(dbsql_file_t*, int64_t size);
static int __vfslog_sync(dbsql_file_t*, int flags);
static int __vfslog_file_size(dbsql_file_t*, int64_t *size);
static int __vfslog_lock(dbsql_file_t*, int);
static int __vfslog_unlock(dbsql_file_t*, int);
static int __vfslog_check_reserved_lock(dbsql_file_t*, int *res_out);
static int __vfslog_file_control(dbsql_file_t*, int op, void *arg);
static int __vfslog_sector_size(dbsql_file_t*);
static int __vfslog_device_characteristics(dbsql_file_t*);

static int __vfslog_shm_lock(dbsql_file_t *file, int ofst, int n, int flags);
static int __vfslog_shm_map(dbsql_file_t *file,int,int,int,volatile void **);
static void __vfslog_shm_barrier(dbsql_file_t*);
static int __vfslog_shm_unmap(dbsql_file_t *file, int delete_flag);

/*
** Method declarations for vfslog_vfs.
*/
static int __vfslog_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __vfslog_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __vfslog_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __vfslog_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
static void *__vfslog_dl_open(dbsql_vfs_t*, const char *filename);
static void __vfslog_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*vfslog_dl_sym(dbsql_vfs_t *vfs, void *p, const char*sym))(void);
static void __vfslog_dl_close(dbsql_vfs_t*, void*);
static int __vfslog_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __vfslog_sleep(dbsql_vfs_t*, int microseconds);
static int __vfslog_current_time(dbsql_vfs_t*, double*);

static int __vfslog_get_last_error(dbsql_vfs_t*, int, char *);
static int __vfslog_current_time_int64(dbsql_vfs_t*, int64_t*);

static dbsql_vfs_t vfslog_vfs = {
  1,                              /* i_version */
  sizeof(vfslog_file_t),             /* sz_os_file */
  INST_MAX_PATHNAME,              /* mx_pathname */
  0,                              /* next */
  0,                              /* name */
  0,                              /* app_data */
  __vfslog_open,                     /* x_open */
  __vfslog_delete,                   /* x_delete */
  __vfslog_access,                   /* x_access */
  __vfslog_full_pathname,             /* full_pathname */
  __vfslog_dl_open,                   /* dl_open */
  __vfslog_dl_error,                  /* dl_error */
  vfslog_dl_sym,                    /* dl_sym */
  __vfslog_dl_close,                  /* dl_close */
  __vfslog_randomness,               /* randomness */
  __vfslog_sleep,                    /* x_sleep */
  __vfslog_current_time,              /* current_time */
  __vfslog_get_last_error,             /* get_last_error */
  __vfslog_current_time_int64          /* current_time */
};

static dbsql_io_methods_t vfslog_io_methods = {
  2,                              /* i_version */
  __vfslog_close,                    /* x_close */
  __vfslog_read,                     /* x_read */
  __vfslog_write,                    /* x_write */
  __vfslog_truncate,                 /* truncate */
  __vfslog_sync,                     /* x_sync */
  __vfslog_file_size,                 /* file_size */
  __vfslog_lock,                     /* lock */
  __vfslog_unlock,                   /* unlock */
  __vfslog_check_reserved_lock,        /* check_reserved_lock */
  __vfslog_file_control,              /* file_control */
  __vfslog_sector_size,               /* sector_size */
  __vfslog_device_characteristics,    /* device_characteristics */
  __vfslog_shm_map,                   /* shm_map */
  __vfslog_shm_lock,                  /* shm_lock */
  __vfslog_shm_barrier,               /* shm_barrier */
  __vfslog_shm_unmap                  /* shm_unmap */
};

#ifdef _WIN32
#include <time.h>
static u_int64_t vfslog_time(){
  FILETIME ft;
  u_int64_t u64time = 0;
 
  __get_system_time_as_file_time(&ft);

  u64time |= ft.dw_high_date_time;
  u64time <<= 32;
  u64time |= ft.dw_low_date_time;

  /* ft is 100-nanosecond intervals, we want microseconds */
  return u64time /(u_int64_t)10;
}
#elif !defined(NO_GETTOD)
#include <sys/time.h>
static u_int64_t vfslog_time(){
  struct timeval s_time;
  gettimeofday(&s_time, 0);
  return s_time.tv_usec + (u_int64_t)s_time.tv_sec * 1000000;
}
#else
static u_int64_t vfslog_time(){
  return 0;
}
#endif

static void vfslog_call(dbsql_vfs_t *, int, int, int64_t, int, int, int);
static void vfslog_string(dbsql_vfs_t *, const char *);

/*
** Close an vfslog-file.
*/
static int __vfslog_close(dbsql_file_t *file){
  u_int64_t t;
  int rc = DBSQL_OK;
  vfslog_file_t *p = (vfslog_file_t *)file;

  t = vfslog_time();
  if( p->real->methods ){
    rc = p->real->methods->x_close(p->real);
  }
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_CLOSE, p->i_file_id, t, rc, 0, 0);
  return rc;
}

/*
** Read data from an vfslog-file.
*/
static int __vfslog_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->x_read(p->real, buf, i_amt, i_ofst);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_READ, p->i_file_id, t, rc, i_amt, (int)i_ofst);
  return rc;
}

/*
** Write data to an vfslog-file.
*/
static int __vfslog_write(
  dbsql_file_t *file,
  const void *z,
  int i_amt,
  int64_t i_ofst
){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->x_write(p->real, z, i_amt, i_ofst);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_WRITE, p->i_file_id, t, rc, i_amt, (int)i_ofst);
  return rc;
}

/*
** Truncate an vfslog-file.
*/
static int __vfslog_truncate(dbsql_file_t *file, int64_t size){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->truncate(p->real, size);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_TRUNCATE, p->i_file_id, t, rc, 0, (int)size);
  return rc;
}

/*
** Sync an vfslog-file.
*/
static int __vfslog_sync(dbsql_file_t *file, int flags){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->x_sync(p->real, flags);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SYNC, p->i_file_id, t, rc, flags, 0);
  return rc;
}

/*
** Return the current file-size of an vfslog-file.
*/
static int __vfslog_file_size(dbsql_file_t *file, int64_t *size){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->file_size(p->real, size);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_FILESIZE, p->i_file_id, t, rc, 0, (int)*size);
  return rc;
}

/*
** Lock an vfslog-file.
*/
static int __vfslog_lock(dbsql_file_t *file, int lock){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->lock(p->real, lock);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_LOCK, p->i_file_id, t, rc, lock, 0);
  return rc;
}

/*
** Unlock an vfslog-file.
*/
static int __vfslog_unlock(dbsql_file_t *file, int lock){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->unlock(p->real, lock);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_UNLOCK, p->i_file_id, t, rc, lock, 0);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an vfslog-file.
*/
static int __vfslog_check_reserved_lock(dbsql_file_t *file, int *res_out){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->check_reserved_lock(p->real, res_out);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_CHECKRESERVEDLOCK, p->i_file_id, t, rc, *res_out, 0);
  return rc;
}

/*
** File control method. For custom operations on an vfslog-file.
*/
static int __vfslog_file_control(dbsql_file_t *file, int op, void *arg){
  vfslog_file_t *p = (vfslog_file_t *)file;
  int rc = p->real->methods->file_control(p->real, op, arg);
  if( op==DBSQL_FCNTL_VFSNAME && rc==DBSQL_OK ){
    *(char**)arg = dbsql_mprintf("vfslog/%z", *(char**)arg);
  }
  return rc;
}

/*
** Return the sector-size in bytes for an vfslog-file.
*/
static int __vfslog_sector_size(dbsql_file_t *file){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->sector_size(p->real);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SECTORSIZE, p->i_file_id, t, rc, 0, 0);
  return rc;
}

/*
** Return the device characteristic flags supported by an vfslog-file.
*/
static int __vfslog_device_characteristics(dbsql_file_t *file){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->device_characteristics(p->real);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_DEVCHAR, p->i_file_id, t, rc, 0, 0);
  return rc;
}

static int __vfslog_shm_lock(dbsql_file_t *file, int ofst, int n, int flags){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->shm_lock(p->real, ofst, n, flags);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SHMLOCK, p->i_file_id, t, rc, 0, 0);
  return rc;
}
static int __vfslog_shm_map(
  dbsql_file_t *file, 
  int i_region, 
  int sz_region, 
  int is_write, 
  volatile void **pp
){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->shm_map(p->real, i_region, sz_region, is_write, pp);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SHMMAP, p->i_file_id, t, rc, 0, 0);
  return rc;
}
static void __vfslog_shm_barrier(dbsql_file_t *file){
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  p->real->methods->shm_barrier(p->real);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SHMBARRIER, p->i_file_id, t, DBSQL_OK, 0, 0);
}
static int __vfslog_shm_unmap(dbsql_file_t *file, int delete_flag){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  t = vfslog_time();
  rc = p->real->methods->shm_unmap(p->real, delete_flag);
  t = vfslog_time() - t;
  vfslog_call(p->vfslog, OS_SHMUNMAP, p->i_file_id, t, rc, 0, 0);
  return rc;
}


/*
** Open an vfslog file handle.
*/
static int __vfslog_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  int rc;
  u_int64_t t;
  vfslog_file_t *p = (vfslog_file_t *)file;
  vfslog_vfs_t *p_log = (vfslog_vfs_t *)vfs;

  file->methods = &vfslog_io_methods;
  p->real = (dbsql_file_t *)&p[1];
  p->vfslog = vfs;
  p->i_file_id = ++p_log->i_next_file_id;

  t = vfslog_time();
  rc = REALVFS(vfs)->x_open(REALVFS(vfs), name, p->real, flags, out_flags);
  t = vfslog_time() - t;

  vfslog_call(vfs, OS_OPEN, p->i_file_id, t, rc, 0, 0);
  vfslog_string(vfs, name);
  return rc;
}

/*
** Delete the file located at path. If the dir_sync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int __vfslog_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  int rc;
  u_int64_t t;
  t = vfslog_time();
  rc = REALVFS(vfs)->x_delete(REALVFS(vfs), path, dir_sync);
  t = vfslog_time() - t;
  vfslog_call(vfs, OS_DELETE, 0, t, rc, dir_sync, 0);
  vfslog_string(vfs, path);
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int __vfslog_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  int rc;
  u_int64_t t;
  t = vfslog_time();
  rc = REALVFS(vfs)->x_access(REALVFS(vfs), path, flags, res_out);
  t = vfslog_time() - t;
  vfslog_call(vfs, OS_ACCESS, 0, t, rc, flags, *res_out);
  vfslog_string(vfs, path);
  return rc;
}

/*
** Populate buffer out with the full canonical pathname corresponding
** to the pathname in path. out is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int __vfslog_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  return REALVFS(vfs)->full_pathname(REALVFS(vfs), path, n_out, out);
}

/*
** Open the dynamic library located at path and return a handle.
*/
static void *__vfslog_dl_open(dbsql_vfs_t *vfs, const char *path){
  return REALVFS(vfs)->dl_open(REALVFS(vfs), path);
}

/*
** Populate the buffer errmsg (size n_byte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void __vfslog_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  REALVFS(vfs)->dl_error(REALVFS(vfs), n_byte, errmsg);
}

/*
** Return a pointer to the symbol symbol in the dynamic library handle.
*/
static void (*vfslog_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return REALVFS(vfs)->dl_sym(REALVFS(vfs), p, sym);
}

/*
** Close the dynamic library handle handle.
*/
static void __vfslog_dl_close(dbsql_vfs_t *vfs, void *handle){
  REALVFS(vfs)->dl_close(REALVFS(vfs), handle);
}

/*
** Populate the buffer pointed to by buf_out with n_byte bytes of 
** random data.
*/
static int __vfslog_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return REALVFS(vfs)->randomness(REALVFS(vfs), n_byte, buf_out);
}

/*
** Sleep for n_micro microseconds. Return the number of microseconds 
** actually slept.
*/
static int __vfslog_sleep(dbsql_vfs_t *vfs, int n_micro){
  return REALVFS(vfs)->x_sleep(REALVFS(vfs), n_micro);
}

/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __vfslog_current_time(dbsql_vfs_t *vfs, double *time_out){
  return REALVFS(vfs)->current_time(REALVFS(vfs), time_out);
}

static int __vfslog_get_last_error(dbsql_vfs_t *vfs, int a, char *b){
  return REALVFS(vfs)->get_last_error(REALVFS(vfs), a, b);
}
static int __vfslog_current_time_int64(dbsql_vfs_t *vfs, int64_t *p){
  return REALVFS(vfs)->current_time_int64(REALVFS(vfs), p);
}

static void vfslog_flush(vfslog_vfs_t *p){
#ifdef DBSQL_TEST
  extern int dbsql_io_error_pending;
  extern int dbsql_io_error_persist;
  extern int dbsql_diskfull_pending;

  int pending = dbsql_io_error_pending;
  int persist = dbsql_io_error_persist;
  int diskfull = dbsql_diskfull_pending;

  dbsql_io_error_pending = 0;
  dbsql_io_error_persist = 0;
  dbsql_diskfull_pending = 0;
#endif

  if( p->n_buf ){
    p->p_log->methods->x_write(p->p_log, p->bufs, p->n_buf, p->i_offset);
    p->i_offset += p->n_buf;
    p->n_buf = 0;
  }

#ifdef DBSQL_TEST
  dbsql_io_error_pending = pending;
  dbsql_io_error_persist = persist;
  dbsql_diskfull_pending = diskfull;
#endif
}

static void put32bits(unsigned char *p, unsigned int v){
  p[0] = v>>24;
  p[1] = (unsigned char)(v>>16);
  p[2] = (unsigned char)(v>>8);
  p[3] = (unsigned char)v;
}

static void vfslog_call(
  dbsql_vfs_t *vfs,
  int event,
  int i_fileid,
  int64_t n_click,
  int return_code,
  int size,
  int offset
){
  vfslog_vfs_t *p = (vfslog_vfs_t *)vfs;
  unsigned char *rec;
  if( (24+p->n_buf)>sizeof(p->bufs) ){
    vfslog_flush(p);
  }
  rec = (unsigned char *)&p->bufs[p->n_buf];
  put32bits(&rec[0], event);
  put32bits(&rec[4], i_fileid);
  put32bits(&rec[8], (unsigned int)(n_click&0xffff));
  put32bits(&rec[12], return_code);
  put32bits(&rec[16], size);
  put32bits(&rec[20], offset);
  p->n_buf += 24;
}

static void vfslog_string(dbsql_vfs_t *vfs, const char *str){
  vfslog_vfs_t *p = (vfslog_vfs_t *)vfs;
  unsigned char *rec;
  int n_str = str ? (int)strlen(str) : 0;
  if( (4+n_str+p->n_buf)>sizeof(p->bufs) ){
    vfslog_flush(p);
  }
  rec = (unsigned char *)&p->bufs[p->n_buf];
  put32bits(&rec[0], n_str);
  if( str ){
    memcpy(&rec[4], str, n_str);
  }
  p->n_buf += (4 + n_str);
}

static void vfslog_finalize(vfslog_vfs_t *p){
  if( p->p_log->methods ){
    vfslog_flush(p);
    p->p_log->methods->x_close(p->p_log);
  }
  dbsql_free(p);
}

int dbsql_vfslog_finalize(const char *z_vfs){
  dbsql_vfs_t *vfs;
  vfs = dbsql_vfs_find(z_vfs);
  if( !vfs || vfs->x_open!=__vfslog_open ){
    return DBSQL_ERROR;
  } 
  dbsql_vfs_unregister(vfs);
  vfslog_finalize((vfslog_vfs_t *)vfs);
  return DBSQL_OK;
}

int dbsql_vfslog_new(
  const char *vfs,               /* New VFS name */
  const char *parent_vfs,         /* Parent VFS name (or NULL) */
  const char *z_log                /* Log file name */
){
  vfslog_vfs_t *p;
  dbsql_vfs_t *parent;
  int n_byte;
  int flags;
  int rc;
  char *file;
  int __n_vfs;

  parent = dbsql_vfs_find(parent_vfs);
  if( !parent ){
    return DBSQL_ERROR;
  }

  __n_vfs = (int)strlen(vfs);
  n_byte = sizeof(vfslog_vfs_t) + parent->sz_os_file + __n_vfs+1+parent->mx_pathname+1;
  p = (vfslog_vfs_t *)dbsql_malloc(n_byte);
  memset(p, 0, n_byte);

  p->vfs = parent;
  p->p_log = (dbsql_file_t *)&p[1];
  memcpy(&p->base, &vfslog_vfs, sizeof(dbsql_vfs_t));
  p->base.name = &((char *)p->p_log)[parent->sz_os_file];
  p->base.sz_os_file += parent->sz_os_file;
  memcpy((char *)p->base.name, vfs, __n_vfs);

  file = (char *)&p->base.name[__n_vfs+1];
  parent->full_pathname(parent, z_log, parent->mx_pathname, file);

  flags = DBSQL_OPEN_READWRITE|DBSQL_OPEN_CREATE|DBSQL_OPEN_SUPER_JOURNAL;
  parent->x_delete(parent, file, 0);
  rc = parent->x_open(parent, file, p->p_log, flags, &flags);
  if( rc==DBSQL_OK ){
    memcpy(p->bufs, "dbsql_ostrace1......", 20);
    p->i_offset = 0;
    p->n_buf = 20;
    rc = dbsql_vfs_register((dbsql_vfs_t *)p, 1);
  }
  if( rc ){
    vfslog_finalize(p);
  }
  return rc;
}

int dbsql_vfslog_annotate(const char *z_vfs, const char *msg){
  dbsql_vfs_t *vfs;
  vfs = dbsql_vfs_find(z_vfs);
  if( !vfs || vfs->x_open!=__vfslog_open ){
    return DBSQL_ERROR;
  } 
  vfslog_call(vfs, OS_ANNOTATE, 0, 0, 0, 0, 0);
  vfslog_string(vfs, msg);
  return DBSQL_OK;
}

static const char *vfslog_eventname(int event){
  const char *z_event = 0;

  switch( event ){
    case OS_CLOSE:             z_event = "xClose"; break;
    case OS_READ:              z_event = "xRead"; break;
    case OS_WRITE:             z_event = "xWrite"; break;
    case OS_TRUNCATE:          z_event = "xTruncate"; break;
    case OS_SYNC:              z_event = "xSync"; break;
    case OS_FILESIZE:          z_event = "xFilesize"; break;
    case OS_LOCK:              z_event = "xLock"; break;
    case OS_UNLOCK:            z_event = "xUnlock"; break;
    case OS_CHECKRESERVEDLOCK: z_event = "xCheckResLock"; break;
    case OS_FILECONTROL:       z_event = "xFileControl"; break;
    case OS_SECTORSIZE:        z_event = "xSectorSize"; break;
    case OS_DEVCHAR:           z_event = "xDeviceChar"; break;
    case OS_OPEN:              z_event = "xOpen"; break;
    case OS_DELETE:            z_event = "xDelete"; break;
    case OS_ACCESS:            z_event = "xAccess"; break;
    case OS_FULLPATHNAME:      z_event = "xFullPathname"; break;
    case OS_RANDOMNESS:        z_event = "xRandomness"; break;
    case OS_SLEEP:             z_event = "xSleep"; break;
    case OS_CURRENTTIME:       z_event = "xCurrentTime"; break;

    case OS_SHMUNMAP:          z_event = "xShmUnmap"; break;
    case OS_SHMLOCK:           z_event = "xShmLock"; break;
    case OS_SHMBARRIER:        z_event = "xShmBarrier"; break;
    case OS_SHMMAP:            z_event = "xShmMap"; break;

    case OS_ANNOTATE:          z_event = "annotation"; break;
  }

  return z_event;
}

typedef struct vfslog_vtab_t vfslog_vtab_t;
typedef struct vfslog_csr_t vfslog_csr_t;

/*
** Virtual table type for the vfslog reader module.
*/
struct vfslog_vtab_t {
  dbsql_vtab_t base;              /* Base class */
  dbsql_file_t *fd;              /* File descriptor open on vfslog file */
  int64_t n_byte;            /* Size of file in bytes */
  char *z_file;                    /* File name for fd */
};

/*
** Virtual table cursor type for the vfslog reader module.
*/
struct vfslog_csr_t {
  dbsql_vtab_cursor_t base;       /* Base class */
  int64_t i_rowid;           /* Current rowid. */
  int64_t i_offset;          /* Offset of next record in file */
  char *transient;               /* Transient 'file' string */
  int n_file;                      /* Size of array files[] */
  char **files;                  /* File strings */
  unsigned char bufs[1024];       /* Current vfs log entry (read from file) */
};

static unsigned int get32bits(unsigned char *p){
  return (p[0]<<24) + (p[1]<<16) + (p[2]<<8) + p[3];
}

/*
** The argument must point to a buffer containing a nul-terminated string.
** If the string begins with an SQL quote character it is overwritten by
** the dequoted version. Otherwise the buffer is left unmodified.
*/
static void dequote(char *z){
  char quote;                     /* Quote character (if any ) */
  quote = z[0];
  if( quote=='[' || quote=='\'' || quote=='"' || quote=='`' ){
    int i_in = 1;                  /* index_t of next byte to read from input */
    int i_out = 0;                 /* index_t of next byte to write to output */
    if( quote=='[' ) quote = ']';  
    while( z[i_in] ){
      if( z[i_in]==quote ){
        if( z[i_in+1]!=quote ) break;
        z[i_out++] = quote;
        i_in += 2;
      }else{
        z[i_out++] = z[i_in++];
      }
    }
    z[i_out] = '\0';
  }
}

#ifndef DBSQL_OMIT_VIRTUALTABLE
/*
** Connect to or create a vfslog virtual table.
*/
static int __vlog_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  dbsql_vfs_t *vfs;              /* VFS used to read log file */
  int flags;                      /* flags passed to vfs->x_open() */
  vfslog_vtab_t *p;
  int rc;
  int n_byte;
  char *file;

  *vtab = 0;
  vfs = dbsql_vfs_find(0);
  n_byte = sizeof(vfslog_vtab_t) + vfs->sz_os_file + vfs->mx_pathname;
  p = dbsql_malloc(n_byte);
  if( p==0 ) return DBSQL_NOMEM;
  memset(p, 0, n_byte);

  p->fd = (dbsql_file_t *)&p[1];
  p->z_file = &((char *)p->fd)[vfs->sz_os_file];

  file = dbsql_mprintf("%s", argv[3]);
  if( !file ){
    dbsql_free(p);
    return DBSQL_NOMEM;
  }
  dequote(file);
  vfs->full_pathname(vfs, file, vfs->mx_pathname, p->z_file);
  dbsql_free(file);

  flags = DBSQL_OPEN_READWRITE|DBSQL_OPEN_SUPER_JOURNAL;
  rc = vfs->x_open(vfs, p->z_file, p->fd, flags, &flags);

  if( rc==DBSQL_OK ){
    p->fd->methods->file_size(p->fd, &p->n_byte);
    dbsql_declare_vtab(db, 
        "CREATE TABLE xxx(event, file, click, rc, size, offset)"
    );
    *vtab = &p->base;
  }else{
    dbsql_free(p);
  }

  return rc;
}

/*
** There is no "best-index". This virtual table always does a linear
** scan of the binary VFS log file.
*/
static int __vlog_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  idx_info->estimated_cost = 10.0;
  return DBSQL_OK;
}

/*
** Disconnect from or destroy a vfslog virtual table.
*/
static int __vlog_disconnect(dbsql_vtab_t *vtab){
  vfslog_vtab_t *p = (vfslog_vtab_t *)vtab;
  if( p->fd->methods ){
    p->fd->methods->x_close(p->fd);
    p->fd->methods = 0;
  }
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Open a new vfslog cursor.
*/
static int __vlog_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  vfslog_csr_t *csr;                /* Newly allocated cursor object */

  csr = dbsql_malloc(sizeof(vfslog_csr_t));
  if( !csr ) return DBSQL_NOMEM;
  memset(csr, 0, sizeof(vfslog_csr_t));
  *cursor = &csr->base;
  return DBSQL_OK;
}

/*
** Close a vfslog cursor.
*/
static int __vlog_close(dbsql_vtab_cursor_t *cursor){
  vfslog_csr_t *p = (vfslog_csr_t *)cursor;
  int i;
  for(i=0; i<p->n_file; i++){
    dbsql_free(p->files[i]);
  }
  dbsql_free(p->files);
  dbsql_free(p->transient);
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Move a vfslog cursor to the next entry in the file.
*/
static int __vlog_next(dbsql_vtab_cursor_t *cursor){
  vfslog_csr_t *csr = (vfslog_csr_t *)cursor;
  vfslog_vtab_t *p = (vfslog_vtab_t *)cursor->vtab;
  int rc = DBSQL_OK;
  int n_read;

  dbsql_free(csr->transient);
  csr->transient = 0;

  n_read = 24;
  if( csr->i_offset+n_read<=p->n_byte ){
    int event;
    rc = p->fd->methods->x_read(p->fd, csr->bufs, n_read, csr->i_offset);

    event = get32bits(csr->bufs);
    if( (rc==DBSQL_OK)
     && (event==OS_OPEN || event==OS_DELETE || event==OS_ACCESS) 
    ){
      char buf[4];
      rc = p->fd->methods->x_read(p->fd, buf, 4, csr->i_offset+n_read);
      n_read += 4;
      if( rc==DBSQL_OK ){
        int n_str = get32bits((unsigned char *)buf);
        char *str = dbsql_malloc(n_str+1);
        rc = p->fd->methods->x_read(p->fd, str, n_str, csr->i_offset+n_read);
        str[n_str] = '\0';
        n_read += n_str;

        if( event==OS_OPEN ){
          int i_fileid = get32bits(&csr->bufs[4]);
          if( i_fileid>=csr->n_file ){
            int n_new = sizeof(csr->files[0])*(i_fileid+1);
            csr->files = (char **)dbsql_realloc(csr->files, n_new);
            n_new -= sizeof(csr->files[0])*csr->n_file;
            memset(&csr->files[csr->n_file], 0, n_new);
            csr->n_file = i_fileid+1;
          }
          dbsql_free(csr->files[i_fileid]);
          csr->files[i_fileid] = str;
        }else{
          csr->transient = str;
        }
      }
    }
  }

  csr->i_rowid += 1;
  csr->i_offset += n_read;
  return rc;
}

static int __vlog_eof(dbsql_vtab_cursor_t *cursor){
  vfslog_csr_t *csr = (vfslog_csr_t *)cursor;
  vfslog_vtab_t *p = (vfslog_vtab_t *)cursor->vtab;
  return (csr->i_offset>=p->n_byte);
}

static int __vlog_filter(
  dbsql_vtab_cursor_t *cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  vfslog_csr_t *csr = (vfslog_csr_t *)cursor;
  csr->i_rowid = 0;
  csr->i_offset = 20;
  return __vlog_next(cursor);
}

static int __vlog_column(
  dbsql_vtab_cursor_t *cursor, 
  dbsql_func_t *ctx, 
  int i
){
  unsigned int val;
  vfslog_csr_t *csr = (vfslog_csr_t *)cursor;

  assert( i<7 );
  val = get32bits(&csr->bufs[4*i]);

  switch( i ){
    case 0: {
      dbsql_result_text(ctx, vfslog_eventname(val), -1, DBSQL_STATIC);
      break;
    }
    case 1: {
      char *str = csr->transient;
      if( val!=0 && val<(unsigned)csr->n_file ){
        str = csr->files[val];
      }
      dbsql_result_text(ctx, str, -1, DBSQL_TRANSIENT);
      break;
    }
    default:
      dbsql_result_int(ctx, val);
      break;
  }

  return DBSQL_OK;
}

static int __vlog_rowid(dbsql_vtab_cursor_t *cursor, int64_t *rowid){
  vfslog_csr_t *csr = (vfslog_csr_t *)cursor;
  *rowid = csr->i_rowid;
  return DBSQL_OK;
}

int dbsql_vfslog_register(DBSQL *db){
  static dbsql_module_t vfslog_module = {
    0,                            /* i_version */
    __vlog_connect,                /* create */
    __vlog_connect,                /* connect */
    __vlog_best_index,              /* best_index */
    __vlog_disconnect,             /* disconnect */
    __vlog_disconnect,             /* destroy */
    __vlog_open,                   /* x_open - open a cursor */
    __vlog_close,                  /* x_close - close a cursor */
    __vlog_filter,                 /* filter - configure scan constraints */
    __vlog_next,                   /* next - advance a cursor */
    __vlog_eof,                    /* eof - check for end of scan */
    __vlog_column,                 /* column - read data */
    __vlog_rowid,                  /* rowid - read data */
    0,                            /* update */
    0,                            /* begin */
    0,                            /* x_sync */
    0,                            /* commit */
    0,                            /* rollback */
    0,                            /* find_method */
    0,                            /* rename */
    0,                            /* savepoint */
    0,                            /* release */
    0,                            /* rollback_to */
    0,                            /* shadow_name */
    0                             /* integrity */
 };

  dbsql_create_module(db, "vfslog", &vfslog_module, 0);
  return DBSQL_OK;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

/**************************************************************************
***************************************************************************
** Tcl interface starts here.
*/

#if defined(DBSQL_TEST) || defined(TCLSH)

#include "tcl_dbsql.h"

static int DBSQL_TCLAPI test_vfslog(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct dbsql_db { DBSQL *db; };
  DBSQL *db;
  Tcl_CmdInfo cmd_info;
  int rc = DBSQL_ERROR;

  static const char *strs[] = { "annotate", "finalize", "new",  "register", 0 };
  enum VL_enum { VL_ANNOTATE, VL_FINALIZE, VL_NEW, VL_REGISTER };
  int i_sub;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND ...");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], strs, "sub-command", 0, &i_sub) ){
    return TCL_ERROR;
  }

  switch( (enum VL_enum)i_sub ){
    case VL_ANNOTATE: {
      char *vfs;
      char *msg;
      if( objc!=4 ){
        Tcl_WrongNumArgs(interp, 3, objv, "VFS");
        return TCL_ERROR;
      }
      vfs = Tcl_GetString(objv[2]);
      msg = Tcl_GetString(objv[3]);
      rc = dbsql_vfslog_annotate(vfs, msg);
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, "failed", (char*)0);
        return TCL_ERROR;
      }
      break;
    }
    case VL_FINALIZE: {
      char *vfs;
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "VFS");
        return TCL_ERROR;
      }
      vfs = Tcl_GetString(objv[2]);
      rc = dbsql_vfslog_finalize(vfs);
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, "failed", (char*)0);
        return TCL_ERROR;
      }
      break;
    };

    case VL_NEW: {
      char *vfs;
      char *parent;
      char *z_log;
      if( objc!=5 ){
        Tcl_WrongNumArgs(interp, 2, objv, "VFS PARENT LOGFILE");
        return TCL_ERROR;
      }
      vfs = Tcl_GetString(objv[2]);
      parent = Tcl_GetString(objv[3]);
      z_log = Tcl_GetString(objv[4]);
      if( *parent=='\0' ) parent = 0;
      rc = dbsql_vfslog_new(vfs, parent, z_log);
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, "failed", (char*)0);
        return TCL_ERROR;
      }
      break;
    };

    case VL_REGISTER: {
      char *z_db;
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "DB");
        return TCL_ERROR;
      }
#ifdef DBSQL_OMIT_VIRTUALTABLE
      Tcl_AppendResult(interp, "vfslog not available because of "
                               "DBSQL_OMIT_VIRTUALTABLE", (void*)0);
      return TCL_ERROR;
#else
      z_db = Tcl_GetString(objv[2]);
      if( Tcl_GetCommandInfo(interp, z_db, &cmd_info) ){
        db = ((struct dbsql_db*)cmd_info.objClientData)->db;
        rc = dbsql_vfslog_register(db);
      }
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, "bad dbsql handle: ", z_db, (void*)0);
        return TCL_ERROR;
      }
      break;
#endif
    }
  }

  return TCL_OK;
}

int DbsqltestOsinst_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "vfslog", test_vfslog, 0, 0);
  return TCL_OK;
}

#endif /* DBSQL_TEST */
