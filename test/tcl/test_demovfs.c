/*
** 2010 April 7
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file implements an example of a simple VFS implementation that 
** omits complex features often not required or not possible on embedded
** platforms.  Code is included to buffer writes to the journal file, 
** which can be a significant performance improvement on some embedded
** platforms.
**
** OVERVIEW
**
**   The code in this file implements a minimal dbsql VFS that can be 
**   used on Linux and other posix-like operating systems. The following 
**   system calls are used:
**
**    File-system: access(), unlink(), getcwd()
**    File IO:     open(), read(), write(), fsync(), close(), fstat()
**    Other:       sleep(), usleep(), time()
**
**   The following VFS features are omitted:
**
**     1. File locking. The user must ensure that there is at most one
**        connection to each database when using this VFS. Multiple
**        connections to a single shared-cache count as a single connection
**        for the purposes of the previous statement.
**
**     2. The loading of dynamic extensions (shared libraries).
**
**     3. Temporary files. The user must configure dbsql to use in-memory
**        temp files when using this VFS. The easiest way to do this is to
**        compile with:
**
**          -DSQLITE_TEMP_STORE=3
**
**     4. File truncation. As of version 3.6.24, dbsql may run without
**        a working truncate() call, providing the user does not configure
**        dbsql to use "journal_mode=truncate", or use both
**        "journal_mode=persist" and ATTACHed databases.
**
**   It is assumed that the system uses UNIX-like path-names. Specifically,
**   that '/' characters are used to separate path components and that
**   a path-name is a relative path unless it begins with a '/'. And that
**   no UTF-8 encoded paths are greater than 512 bytes in length.
**
** JOURNAL WRITE-BUFFERING
**
**   To commit a transaction to the database, dbsql first writes rollback
**   information into the journal file. This usually consists of 4 steps:
**
**     1. The rollback information is sequentially written into the journal
**        file, starting at the start of the file.
**     2. The journal file is synced to disk.
**     3. A modification is made to the first few bytes of the journal file.
**     4. The journal file is synced to disk again.
**
**   Most of the data is written in step 1 using a series of calls to the
**   VFS x_write() method. The buffers passed to the x_write() calls are of
**   various sizes. For example, as of version 3.6.24, when committing a 
**   transaction that modifies 3 pages of a database file that uses 4096 
**   byte pages residing on a media with 512 byte sectors, dbsql makes 
**   eleven calls to the x_write() method to create the rollback journal, 
**   as follows:
**
**             Write offset | Bytes written
**             ----------------------------
**                        0            512
**                      512              4
**                      516           4096
**                     4612              4
**                     4616              4
**                     4620           4096
**                     8716              4
**                     8720              4
**                     8724           4096
**                    12820              4
**             ++++++++++++SYNC+++++++++++
**                        0             12
**             ++++++++++++SYNC+++++++++++
**
**   On many operating systems, this is an efficient way to write to a file.
**   However, on some embedded systems that do not cache writes in OS 
**   buffers it is much more efficient to write data in blocks that are
**   an integer multiple of the sector-size in size and aligned at the
**   start of a sector.
**
**   To work around this, the code in this file allocates a fixed size
**   buffer of DBSQL_DEMOVFS_BUFFERSZ using dbsql_malloc() whenever a 
**   journal file is opened. It uses the buffer to coalesce sequential
**   writes into aligned DBSQL_DEMOVFS_BUFFERSZ blocks. When dbsql
**   invokes the x_sync() method to sync the contents of the file to disk,
**   all accumulated data is written out, even if it does not constitute
**   a complete block. This means the actual IO to create the rollback 
**   journal for the example transaction above is this:
**
**             Write offset | Bytes written
**             ----------------------------
**                        0           8192
**                     8192           4632
**             ++++++++++++SYNC+++++++++++
**                        0             12
**             ++++++++++++SYNC+++++++++++
**
**   Much more efficient if the underlying OS is not caching write 
**   operations.
*/

#if !defined(DBSQL_TEST) || DBSQL_OS_UNIX

#include "dbsql.h"

#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/param.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

/*
** Size of the write buffer used by journal files in bytes.
*/
#ifndef DBSQL_DEMOVFS_BUFFERSZ
# define DBSQL_DEMOVFS_BUFFERSZ 8192
#endif

/*
** The maximum pathname length supported by this VFS.
*/
#define MAXPATHNAME 512

/*
** When using this VFS, the dbsql_file_t* handles that dbsql uses are
** actually pointers to instances of type demo_file_t.
*/
typedef struct demo_file_t demo_file_t;
struct demo_file_t {
  dbsql_file_t base;              /* Base class. Must be first. */
  int fd;                         /* File descriptor */

  char *buffers;                  /* Pointer to malloc'd buffer */
  int n_buffer;                    /* Valid bytes of data in buffer */
  int64_t i_buffer_ofst;      /* Offset in file of buffer[0] */
};

/*
** Write directly to the file passed as the first argument. Even if the
** file has a write-buffer (demo_file_t.buffers), ignore it.
*/
static int __demo_direct_write(
  demo_file_t *p,                    /* File handle */
  const void *buf,               /* Buffer containing data to write */
  int i_amt,                       /* Size of data to write in bytes */
  int64_t i_ofst              /* File offset to write to */
){
  off_t ofst;                     /* Return value from lseek() */
  size_t n_write;                  /* Return value from write() */

  ofst = lseek(p->fd, i_ofst, SEEK_SET);
  if( ofst!=i_ofst ){
    return DBSQL_IOERR_WRITE;
  }

  n_write = write(p->fd, buf, i_amt);
  if( n_write!=i_amt ){
    return DBSQL_IOERR_WRITE;
  }

  return DBSQL_OK;
}

/*
** Flush the contents of the demo_file_t.buffers buffer to disk. This is a
** no-op if this particular file does not have a buffer (i.e. it is not
** a journal file) or if the buffer is currently empty.
*/
static int __demo_flush_buffer(demo_file_t *p){
  int rc = DBSQL_OK;
  if( p->n_buffer ){
    rc = __demo_direct_write(p, p->buffers, p->n_buffer, p->i_buffer_ofst);
    p->n_buffer = 0;
  }
  return rc;
}

/*
** Close a file.
*/
static int __demo_close(dbsql_file_t *file){
  int rc;
  demo_file_t *p = (demo_file_t*)file;
  rc = __demo_flush_buffer(p);
  dbsql_free(p->buffers);
  close(p->fd);
  return rc;
}

/*
** Read data from a file.
*/
static int __demo_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  demo_file_t *p = (demo_file_t*)file;
  off_t ofst;                     /* Return value from lseek() */
  int n_read;                      /* Return value from read() */
  int rc;                         /* Return code from __demo_flush_buffer() */

  /* Flush any data in the write buffer to disk in case this operation
  ** is trying to read data the file-region currently cached in the buffer.
  ** It would be possible to detect this case and possibly save an 
  ** unnecessary write here, but in practice dbsql will rarely read from
  ** a journal file when there is data cached in the write-buffer.
  */
  rc = __demo_flush_buffer(p);
  if( rc!=DBSQL_OK ){
    return rc;
  }

  ofst = lseek(p->fd, i_ofst, SEEK_SET);
  if( ofst!=i_ofst ){
    return DBSQL_IOERR_READ;
  }
  n_read = read(p->fd, buf, i_amt);

  if( n_read==i_amt ){
    return DBSQL_OK;
  }else if( n_read>=0 ){
    if( n_read<i_amt ){
      memset(&((char*)buf)[n_read], 0, i_amt-n_read);
    }
    return DBSQL_IOERR_SHORT_READ;
  }

  return DBSQL_IOERR_READ;
}

/*
** Write data to a crash-file.
*/
static int __demo_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  demo_file_t *p = (demo_file_t*)file;
  
  if( p->buffers ){
    char *z = (char *)buf;       /* Pointer to remaining data to write */
    int n = i_amt;                 /* Number of bytes at z */
    int64_t i = i_ofst;      /* File offset to write to */

    while( n>0 ){
      int n_copy;                  /* Number of bytes to copy into buffer */

      /* If the buffer is full, or if this data is not being written directly
      ** following the data already buffered, flush the buffer. Flushing
      ** the buffer is a no-op if it is empty.  
      */
      if( p->n_buffer==DBSQL_DEMOVFS_BUFFERSZ || p->i_buffer_ofst+p->n_buffer!=i ){
        int rc = __demo_flush_buffer(p);
        if( rc!=DBSQL_OK ){
          return rc;
        }
      }
      assert( p->n_buffer==0 || p->i_buffer_ofst+p->n_buffer==i );
      p->i_buffer_ofst = i - p->n_buffer;

      /* Copy as much data as possible into the buffer. */
      n_copy = DBSQL_DEMOVFS_BUFFERSZ - p->n_buffer;
      if( n_copy>n ){
        n_copy = n;
      }
      memcpy(&p->buffers[p->n_buffer], z, n_copy);
      p->n_buffer += n_copy;

      n -= n_copy;
      i += n_copy;
      z += n_copy;
    }
  }else{
    return __demo_direct_write(p, buf, i_amt, i_ofst);
  }

  return DBSQL_OK;
}

/*
** Truncate a file. This is a no-op for this VFS (see header comments at
** the top of the file).
*/
static int __demo_truncate(dbsql_file_t *file, int64_t size){
#if 0
  if( ftruncate(((demo_file_t *)file)->fd, size) ) return DBSQL_IOERR_TRUNCATE;
#endif
  return DBSQL_OK;
}

/*
** Sync the contents of the file to the persistent media.
*/
static int __demo_sync(dbsql_file_t *file, int flags){
  demo_file_t *p = (demo_file_t*)file;
  int rc;

  rc = __demo_flush_buffer(p);
  if( rc!=DBSQL_OK ){
    return rc;
  }

  rc = fsync(p->fd);
  return (rc==0 ? DBSQL_OK : DBSQL_IOERR_FSYNC);
}

/*
** Write the size of the file in bytes to *size.
*/
static int __demo_file_size(dbsql_file_t *file, int64_t *size){
  demo_file_t *p = (demo_file_t*)file;
  int rc;                         /* Return code from fstat() call */
  struct stat s_stat;              /* Output of fstat() call */

  /* Flush the contents of the buffer to disk. As with the flush in the
  ** __demo_read() method, it would be possible to avoid this and save a write
  ** here and there. But in practice this comes up so infrequently it is
  ** not worth the trouble.
  */
  rc = __demo_flush_buffer(p);
  if( rc!=DBSQL_OK ){
    return rc;
  }

  rc = fstat(p->fd, &s_stat);
  if( rc!=0 ) return DBSQL_IOERR_FSTAT;
  *size = s_stat.st_size;
  return DBSQL_OK;
}

/*
** Locking functions. The lock() and unlock() methods are both no-ops.
** The check_reserved_lock() always indicates that no other process holds
** a reserved lock on the database file. This ensures that if a hot-journal
** file is found in the file-system it is rolled back.
*/
static int __demo_lock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}
static int __demo_unlock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}
static int __demo_check_reserved_lock(dbsql_file_t *file, int *res_out){
  *res_out = 0;
  return DBSQL_OK;
}

/*
** No file_control() verbs are implemented by this VFS.
*/
static int __demo_file_control(dbsql_file_t *file, int op, void *arg){
  return DBSQL_NOTFOUND;
}

/*
** The sector_size() and device_characteristics() methods. These two
** may return special values allowing dbsql to optimize file-system 
** access to some extent. But it is also safe to simply return 0.
*/
static int __demo_sector_size(dbsql_file_t *file){
  return 0;
}
static int __demo_device_characteristics(dbsql_file_t *file){
  return 0;
}

/*
** Open a file handle.
*/
static int __demo_open(
  dbsql_vfs_t *vfs,              /* VFS */
  const char *name,              /* File to open, or 0 for a temp file */
  dbsql_file_t *file,            /* Pointer to demo_file_t struct to populate */
  int flags,                      /* Input DBSQL_OPEN_XXX flags */
  int *out_flags                  /* Output DBSQL_OPEN_XXX flags (or NULL) */
){
  static const dbsql_io_methods_t demoio = {
    1,                            /* i_version */
    __demo_close,                    /* x_close */
    __demo_read,                     /* x_read */
    __demo_write,                    /* x_write */
    __demo_truncate,                 /* truncate */
    __demo_sync,                     /* x_sync */
    __demo_file_size,                 /* file_size */
    __demo_lock,                     /* lock */
    __demo_unlock,                   /* unlock */
    __demo_check_reserved_lock,        /* check_reserved_lock */
    __demo_file_control,              /* file_control */
    __demo_sector_size,               /* sector_size */
    __demo_device_characteristics     /* device_characteristics */
  };

  demo_file_t *p = (demo_file_t*)file; /* Populate this structure */
  int oflags = 0;                 /* flags to pass to open() call */
  char *bufs = 0;

  if( name==0 ){
    return DBSQL_IOERR;
  }

  if( flags&DBSQL_OPEN_MAIN_JOURNAL ){
    bufs = (char *)dbsql_malloc(DBSQL_DEMOVFS_BUFFERSZ);
    if( !bufs ){
      return DBSQL_NOMEM;
    }
  }

  if( flags&DBSQL_OPEN_EXCLUSIVE ) oflags |= O_EXCL;
  if( flags&DBSQL_OPEN_CREATE )    oflags |= O_CREAT;
  if( flags&DBSQL_OPEN_READONLY )  oflags |= O_RDONLY;
  if( flags&DBSQL_OPEN_READWRITE ) oflags |= O_RDWR;

  memset(p, 0, sizeof(demo_file_t));
  p->fd = open(name, oflags, 0600);
  if( p->fd<0 ){
    dbsql_free(bufs);
    return DBSQL_CANTOPEN;
  }
  p->buffers = bufs;

  if( out_flags ){
    *out_flags = flags;
  }
  p->base.methods = &demoio;
  return DBSQL_OK;
}

/*
** Delete the file identified by argument path. If the dir_sync parameter
** is non-zero, then ensure the file-system modification to delete the
** file has been synced to disk before returning.
*/
static int __demo_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  int rc;                         /* Return code */

  rc = unlink(path);
  if( rc!=0 && errno==ENOENT ) return DBSQL_OK;

  if( rc==0 && dir_sync ){
    int dfd;                      /* File descriptor open on directory */
    char *slash;
    char dir[MAXPATHNAME+1];     /* Name of directory containing file path */

    /* Figure out the directory name from the path of the file deleted. */
    dbsql_snprintf(MAXPATHNAME, dir, "%s", path);
    dir[MAXPATHNAME] = '\0';
    slash = strrchr(dir,'/');
    if( slash ){
      /* Open a file-descriptor on the directory. Sync. Close. */
      slash[0] = 0;
      dfd = open(dir, O_RDONLY, 0);
      if( dfd<0 ){
        rc = -1;
      }else{
        rc = fsync(dfd);
        close(dfd);
      }
    }
  }
  return (rc==0 ? DBSQL_OK : DBSQL_IOERR_DELETE);
}

#ifndef F_OK
# define F_OK 0
#endif
#ifndef R_OK
# define R_OK 4
#endif
#ifndef W_OK
# define W_OK 2
#endif

/*
** Query the file-system to see if the named file exists, is readable or
** is both readable and writable.
*/
static int __demo_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  int rc;                         /* access() return code */
  int e_access = F_OK;             /* Second argument to access() */

  assert( flags==DBSQL_ACCESS_EXISTS       /* access(path, F_OK) */
       || flags==DBSQL_ACCESS_READ         /* access(path, R_OK) */
       || flags==DBSQL_ACCESS_READWRITE    /* access(path, R_OK|W_OK) */
  );

  if( flags==DBSQL_ACCESS_READWRITE ) e_access = R_OK|W_OK;
  if( flags==DBSQL_ACCESS_READ )      e_access = R_OK;

  rc = access(path, e_access);
  *res_out = (rc==0);
  return DBSQL_OK;
}

/*
** Argument path points to a nul-terminated string containing a file path.
** If path is an absolute path, then it is copied as is into the output 
** buffer. Otherwise, if it is a relative path, then the equivalent full
** path is written to the output buffer.
**
** This function assumes that paths are UNIX style. Specifically, that:
**
**   1. Path components are separated by a '/'. and 
**   2. Full paths begin with a '/' character.
*/
static int __demo_full_pathname(
  dbsql_vfs_t *vfs,              /* VFS */
  const char *path,              /* Input path (possibly a relative path) */
  int n_path_out,                   /* Size of output buffer in bytes */
  char *path_out                  /* Pointer to output buffer */
){
  char dir[MAXPATHNAME+1];
  if( path[0]=='/' ){
    dir[0] = '\0';
  }else{
    if( getcwd(dir, sizeof(dir))==0 ) return DBSQL_IOERR;
  }
  dir[MAXPATHNAME] = '\0';

  dbsql_snprintf(n_path_out, path_out, "%s/%s", dir, path);
  path_out[n_path_out-1] = '\0';

  return DBSQL_OK;
}

/*
** The following four VFS methods:
**
**   dl_open
**   dl_error
**   dl_sym
**   dl_close
**
** are supposed to implement the functionality needed by dbsql to load
** extensions compiled as shared objects. This simple VFS does not support
** this functionality, so the following functions are no-ops.
*/
static void *__demo_dl_open(dbsql_vfs_t *vfs, const char *path){
  return 0;
}
static void __demo_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  dbsql_snprintf(n_byte, errmsg, "Loadable extensions are not supported");
  errmsg[n_byte-1] = '\0';
}
static void (*demo_dl_sym(dbsql_vfs_t *vfs, void *h, const char *z))(void){
  return 0;
}
static void __demo_dl_close(dbsql_vfs_t *vfs, void *handle){
  return;
}

/*
** Parameter byte points to a buffer n_byte bytes in size. Populate this
** buffer with pseudo-random data.
*/
static int __demo_randomness(dbsql_vfs_t *vfs, int n_byte, char *byte){
  return DBSQL_OK;
}

/*
** Sleep for at least n_micro microseconds. Return the (approximate) number 
** of microseconds slept for.
*/
static int __demo_sleep(dbsql_vfs_t *vfs, int n_micro){
  sleep(n_micro / 1000000);
  usleep(n_micro % 1000000);
  return n_micro;
}

/*
** Set *p_time to the current UTC time expressed as a Julian day. Return
** DBSQL_OK if successful, or an error code otherwise.
**
**   http://en.wikipedia.org/wiki/Julian_day
**
** This implementation is not very good. The current time is rounded to
** an integer number of seconds. Also, assuming time_t is a signed 32-bit 
** value, it will stop working some time in the year 2038 AD (the so-called
** "year 2038" problem that afflicts systems that store time this way). 
*/
static int __demo_current_time(dbsql_vfs_t *vfs, double *p_time){
  time_t t = time(0);
  *p_time = t/86400.0 + 2440587.5; 
  return DBSQL_OK;
}

/*
** This function returns a pointer to the VFS implemented in this file.
** To make the VFS available to dbsql:
**
**   dbsql_vfs_register(dbsql_demovfs(), 0);
*/
dbsql_vfs_t *dbsql_demovfs(void){
  static dbsql_vfs_t demovfs = {
    1,                            /* i_version */
    sizeof(demo_file_t),             /* sz_os_file */
    MAXPATHNAME,                  /* mx_pathname */
    0,                            /* next */
    "demo",                       /* name */
    0,                            /* app_data */
    __demo_open,                     /* x_open */
    __demo_delete,                   /* x_delete */
    __demo_access,                   /* x_access */
    __demo_full_pathname,             /* full_pathname */
    __demo_dl_open,                   /* dl_open */
    __demo_dl_error,                  /* dl_error */
    demo_dl_sym,                    /* dl_sym */
    __demo_dl_close,                  /* dl_close */
    __demo_randomness,               /* randomness */
    __demo_sleep,                    /* x_sleep */
    __demo_current_time,              /* current_time */
  };
  return &demovfs;
}

#endif /* !defined(DBSQL_TEST) || DBSQL_OS_UNIX */


#ifdef DBSQL_TEST

#include "tcl_dbsql.h"

#if DBSQL_OS_UNIX
static int DBSQL_TCLAPI register_demovfs(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_vfs_register(dbsql_demovfs(), 1);
  return TCL_OK;
}
static int DBSQL_TCLAPI unregister_demovfs(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_vfs_unregister(dbsql_demovfs());
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest_demovfs_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "register_demovfs", register_demovfs, 0, 0);
  Tcl_CreateObjCommand(interp, "unregister_demovfs", unregister_demovfs, 0, 0);
  return TCL_OK;
}

#else
int Dbsqltest_demovfs_Init(Tcl_Interp *interp){ return TCL_OK; }
#endif

#endif /* DBSQL_TEST */
