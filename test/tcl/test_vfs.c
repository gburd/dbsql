/*
** 2010 May 05
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
** This file contains the implementation of the Tcl [testvfs] command,
** used to create dbsql VFS implementations with various properties and
** instrumentation to support testing dbsql.
**
**   testvfs VFSNAME ?OPTIONS?
**
** Available options are:
**
**   -noshm      BOOLEAN        (True to omit shm methods. Default false)
**   -default    BOOLEAN        (True to make the vfs default. Default false)
**   -szosfile   INTEGER        (Value for dbsql_vfs_t.sz_os_file)
**   -mxpathname INTEGER        (Value for dbsql_vfs_t.mx_pathname)
**   -iversion   INTEGER        (Value for dbsql_vfs_t.i_version)
*/
#if DBSQL_TEST          /* This file is used for testing only */

#include "dbsql.h"
#include "dbsql_int.h"
#include "tcl_dbsql.h"

typedef struct testvfs_t testvfs_t;
typedef struct testvfs_shm_t testvfs_shm_t;
typedef struct testvfs_buffer_t testvfs_buffer_t;
typedef struct testvfs_file_t testvfs_file_t;
typedef struct testvfs_fd_t testvfs_fd_t;

/*
** An open file handle.
*/
struct testvfs_file_t {
  dbsql_file_t base;              /* Base class.  Must be first */
  testvfs_fd_t *fd;                 /* File data */
};
#define TVFS_GET_FD(file) (((testvfs_file_t *)file)->fd)

struct testvfs_fd_t {
  dbsql_vfs_t *vfs;              /* The VFS */
  const char *filename;          /* Filename as passed to x_open() */
  dbsql_file_t *real;            /* The real, underlying file descriptor */
  Tcl_Obj *shm_id;                /* Shared memory id for Tcl callbacks */

  testvfs_buffer_t *shm;            /* Shared memory buffer */
  u_int32_t excllock;                   /* Mask of exclusive locks */
  u_int32_t sharedlock;                 /* Mask of shared locks */
  testvfs_fd_t *next;               /* Next handle opened on the same file */
};


#define FAULT_INJECT_NONE       0
#define FAULT_INJECT_TRANSIENT  1
#define FAULT_INJECT_PERSISTENT 2

typedef struct test_fault_inject_t test_fault_inject_t;
struct test_fault_inject_t {
  int i_cnt;                       /* Remaining calls before fault injection */
  int fault;                     /* A FAULT_INJECT_* value */
  int n_fail;                      /* Number of faults injected */
};

/*
** An instance of this structure is allocated for each VFS created. The
** dbsql_vfs_t.app_data field of the VFS structure registered with dbsql
** is set to point to it.
*/
struct testvfs_t {
  char *name;                    /* Name of this VFS */
  dbsql_vfs_t *parent;           /* The VFS to use for file IO */
  dbsql_vfs_t *vfs;              /* The testvfs registered with dbsql */
  Tcl_Interp *interp;             /* Interpreter to run script in */
  Tcl_Obj *script;               /* Script to execute */
  testvfs_buffer_t *buffer;         /* List of shared buffers */
  int is_noshm;
  int is_fullshm;

  int mask;                       /* Mask controlling [script] and [ioerr] */

  test_fault_inject_t ioerr_err;
  test_fault_inject_t full_err;
  test_fault_inject_t cantopen_err;

#if 0
  int i_ioerr_cnt;
  int ioerr;
  int n_ioerr_fail;
  int i_full_cnt;
  int fullerr;
  int n_full_fail;
#endif

  int i_devchar;
  int i_sectorsize;
};

/*
** The testvfs_t.mask variable is set to a combination of the following.
** If a bit is clear in testvfs_t.mask, then calls made by dbsql to the 
** corresponding VFS method is ignored for purposes of:
**
**   + Simulating IO errors, and
**   + Invoking the Tcl callback script.
*/
#define TESTVFS_SHMOPEN_MASK      0x00000001
#define TESTVFS_SHMLOCK_MASK      0x00000010
#define TESTVFS_SHMMAP_MASK       0x00000020
#define TESTVFS_SHMBARRIER_MASK   0x00000040
#define TESTVFS_SHMCLOSE_MASK     0x00000080

#define TESTVFS_OPEN_MASK         0x00000100
#define TESTVFS_SYNC_MASK         0x00000200
#define TESTVFS_DELETE_MASK       0x00000400
#define TESTVFS_CLOSE_MASK        0x00000800
#define TESTVFS_WRITE_MASK        0x00001000
#define TESTVFS_TRUNCATE_MASK     0x00002000
#define TESTVFS_ACCESS_MASK       0x00004000
#define TESTVFS_FULLPATHNAME_MASK 0x00008000
#define TESTVFS_READ_MASK         0x00010000
#define TESTVFS_UNLOCK_MASK       0x00020000
#define TESTVFS_LOCK_MASK         0x00040000
#define TESTVFS_CKLOCK_MASK       0x00080000
#define TESTVFS_FCNTL_MASK        0x00100000
#define TESTVFS_SLEEP_MASK        0x00200000

#define TESTVFS_ALL_MASK          0x003FFFFF


#define TESTVFS_MAX_PAGES 1024

/*
** A shared-memory buffer. There is one of these objects for each shared
** memory region opened by clients. If two clients open the same file,
** there are two testvfs_file_t structures but only one testvfs_buffer_t structure.
*/
struct testvfs_buffer_t {
  char *z_file;                    /* Associated file name */
  int pgsz;                       /* Page size */
  u_int8_t *pages[TESTVFS_MAX_PAGES];   /* Array of ckalloc'd pages */
  testvfs_fd_t *file;               /* List of open handles */
  testvfs_buffer_t *next;           /* Next in linked list of all buffers */
};


#define PARENTVFS(x) (((testvfs_t *)((x)->app_data))->parent)

#define TESTVFS_MAX_ARGS 12


/*
** Method declarations for testvfs_file_t.
*/
static int __tvfs_close(dbsql_file_t*);
static int __tvfs_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __tvfs_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __tvfs_truncate(dbsql_file_t*, int64_t size);
static int __tvfs_sync(dbsql_file_t*, int flags);
static int __tvfs_file_size(dbsql_file_t*, int64_t *size);
static int __tvfs_lock(dbsql_file_t*, int);
static int __tvfs_unlock(dbsql_file_t*, int);
static int __tvfs_check_reserved_lock(dbsql_file_t*, int *);
static int __tvfs_file_control(dbsql_file_t*, int op, void *arg);
static int __tvfs_sector_size(dbsql_file_t*);
static int __tvfs_device_characteristics(dbsql_file_t*);

/*
** Method declarations for tvfs_vfs.
*/
static int __tvfs_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __tvfs_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __tvfs_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __tvfs_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
#ifndef DBSQL_OMIT_LOAD_EXTENSION
static void *__tvfs_dl_open(dbsql_vfs_t*, const char *filename);
static void __tvfs_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*tvfs_dl_sym(dbsql_vfs_t*,void*, const char *symbol))(void);
static void __tvfs_dl_close(dbsql_vfs_t*, void*);
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
static int __tvfs_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __tvfs_sleep(dbsql_vfs_t*, int microseconds);
static int __tvfs_current_time(dbsql_vfs_t*, double*);

static int __tvfs_shm_open(dbsql_file_t*);
static int __tvfs_shm_lock(dbsql_file_t*, int , int, int);
static int __tvfs_shm_map(dbsql_file_t*,int,int,int, void volatile **);
static void __tvfs_shm_barrier(dbsql_file_t*);
static int __tvfs_shm_unmap(dbsql_file_t*, int);

static int __tvfs_fetch(dbsql_file_t*, int64_t, int, void**);
static int __tvfs_unfetch(dbsql_file_t*, int64_t, void*);

static dbsql_io_methods_t tvfs_io_methods = {
  3,                              /* i_version */
  __tvfs_close,                      /* x_close */
  __tvfs_read,                       /* x_read */
  __tvfs_write,                      /* x_write */
  __tvfs_truncate,                   /* truncate */
  __tvfs_sync,                       /* x_sync */
  __tvfs_file_size,                   /* file_size */
  __tvfs_lock,                       /* lock */
  __tvfs_unlock,                     /* unlock */
  __tvfs_check_reserved_lock,          /* check_reserved_lock */
  __tvfs_file_control,                /* file_control */
  __tvfs_sector_size,                 /* sector_size */
  __tvfs_device_characteristics,      /* device_characteristics */
  __tvfs_shm_map,                     /* shm_map */
  __tvfs_shm_lock,                    /* shm_lock */
  __tvfs_shm_barrier,                 /* shm_barrier */
  __tvfs_shm_unmap,                   /* shm_unmap */
  __tvfs_fetch,
  __tvfs_unfetch
};

static int __tvfs_result_code(testvfs_t *p, int *rc){
  struct errcode {
    int code;
    const char *z_code;
  } codes[] = {
    { DBSQL_OK,       "DBSQL_OK"     },
    { DBSQL_ERROR,    "DBSQL_ERROR"  },
    { DBSQL_IOERR,    "DBSQL_IOERR"  },
    { DBSQL_LOCKED,   "DBSQL_LOCKED" },
    { DBSQL_BUSY,     "DBSQL_BUSY"   },
    { DBSQL_READONLY, "DBSQL_READONLY"   },
    { DBSQL_READONLY_CANTINIT, "DBSQL_READONLY_CANTINIT"   },
    { DBSQL_NOTFOUND, "DBSQL_NOTFOUND"   },
    { -1,              "DBSQL_OMIT"   },
  };

  const char *z;
  int i;

  z = Tcl_GetStringResult(p->interp);
  for(i=0; i<ARRAY_SIZE(codes); i++){
    if( 0==strcmp(z, codes[i].z_code) ){
      *rc = codes[i].code;
      return 1;
    }
  }

  return 0;
}

static int __tvfs_inject_fault(test_fault_inject_t *p){
  int ret = 0;
  if( p->fault ){
    p->i_cnt--;
    if( p->i_cnt==0 || (p->i_cnt<0 && p->fault==FAULT_INJECT_PERSISTENT ) ){
      ret = 1;
      p->n_fail++;
    }
  }
  return ret;
}


static int __tvfs_inject_ioerr(testvfs_t *p){
  return __tvfs_inject_fault(&p->ioerr_err);
}

static int __tvfs_inject_fullerr(testvfs_t *p){
  return __tvfs_inject_fault(&p->full_err);
}
static int __tvfs_inject_cantopenerr(testvfs_t *p){
  return __tvfs_inject_fault(&p->cantopen_err);
}


static void __tvfs_exec_tcl(
  testvfs_t *p, 
  const char *method,
  Tcl_Obj *arg1,
  Tcl_Obj *arg2,
  Tcl_Obj *arg3,
  Tcl_Obj *arg4
){
  int rc;                         /* Return code from Tcl_EvalObj() */
  Tcl_Obj *eval;
  assert( p->script );

  assert( method );
  assert( p );
  assert( arg2==0 || arg1!=0 );
  assert( arg3==0 || arg2!=0 );

  eval = Tcl_DuplicateObj(p->script);
  Tcl_IncrRefCount(p->script);
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj(method, -1));
  if( arg1 ) Tcl_ListObjAppendElement(p->interp, eval, arg1);
  if( arg2 ) Tcl_ListObjAppendElement(p->interp, eval, arg2);
  if( arg3 ) Tcl_ListObjAppendElement(p->interp, eval, arg3);
  if( arg4 ) Tcl_ListObjAppendElement(p->interp, eval, arg4);

  rc = Tcl_EvalObjEx(p->interp, eval, TCL_EVAL_GLOBAL);
  if( rc!=TCL_OK ){
    Tcl_BackgroundError(p->interp);
    Tcl_ResetResult(p->interp);
  }
}


/*
** Close an tvfs-file.
*/
static int __tvfs_close(dbsql_file_t *file){
  testvfs_file_t *testfile = (testvfs_file_t *)file;
  testvfs_fd_t *fd = testfile->fd;
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;

  if( p->script && p->mask&TESTVFS_CLOSE_MASK ){
    __tvfs_exec_tcl(p, "xClose", 
        Tcl_NewStringObj(fd->filename, -1), fd->shm_id, 0, 0
    );
  }

  if( fd->shm_id ){
    Tcl_DecrRefCount(fd->shm_id);
    fd->shm_id = 0;
  }
  if( file->methods ){
    ckfree((char *)file->methods);
  }
  __dbsql_os_close(fd->real);
  ckfree((char *)fd);
  testfile->fd = 0;
  return DBSQL_OK;
}

/*
** Read data from an tvfs-file.
*/
static int __tvfs_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->script && p->mask&TESTVFS_READ_MASK ){
    __tvfs_exec_tcl(p, "xRead", 
        Tcl_NewStringObj(fd->filename, -1), fd->shm_id, 0, 0
    );
    __tvfs_result_code(p, &rc);
  }
  if( rc==DBSQL_OK && p->mask&TESTVFS_READ_MASK && __tvfs_inject_ioerr(p) ){
    rc = DBSQL_IOERR;
  }
  if( rc==DBSQL_OK ){
    rc = __dbsql_os_read(fd->real, buf, i_amt, i_ofst);
  }
  return rc;
}

/*
** Write data to an tvfs-file.
*/
static int __tvfs_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;

  if( p->script && p->mask&TESTVFS_WRITE_MASK ){
    __tvfs_exec_tcl(p, "xWrite", 
        Tcl_NewStringObj(fd->filename, -1), fd->shm_id, 
        Tcl_NewWideIntObj(i_ofst), Tcl_NewIntObj(i_amt)
    );
    __tvfs_result_code(p, &rc);
    if( rc<0 ) return DBSQL_OK;
  }

  if( rc==DBSQL_OK && __tvfs_inject_fullerr(p) ){
    rc = DBSQL_FULL;
  }
  if( rc==DBSQL_OK && p->mask&TESTVFS_WRITE_MASK && __tvfs_inject_ioerr(p) ){
    rc = DBSQL_IOERR;
  }
  
  if( rc==DBSQL_OK ){
    rc = __dbsql_os_write(fd->real, buf, i_amt, i_ofst);
  }
  return rc;
}

/*
** Truncate an tvfs-file.
*/
static int __tvfs_truncate(dbsql_file_t *file, int64_t size){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;

  if( p->script && p->mask&TESTVFS_TRUNCATE_MASK ){
    __tvfs_exec_tcl(p, "xTruncate", 
        Tcl_NewStringObj(fd->filename, -1), fd->shm_id, 0, 0
    );
    __tvfs_result_code(p, &rc);
  }
  
  if( rc==DBSQL_OK ){
    rc = __dbsql_os_truncate(fd->real, size);
  }
  return rc;
}

/*
** Sync an tvfs-file.
*/
static int __tvfs_sync(dbsql_file_t *file, int flags){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;

  if( p->script && p->mask&TESTVFS_SYNC_MASK ){
    char *z_flags = 0;

    switch( flags ){
      case DBSQL_SYNC_NORMAL:
        z_flags = "normal";
        break;
      case DBSQL_SYNC_FULL:
        z_flags = "full";
        break;
      case DBSQL_SYNC_NORMAL|DBSQL_SYNC_DATAONLY:
        z_flags = "normal|dataonly";
        break;
      case DBSQL_SYNC_FULL|DBSQL_SYNC_DATAONLY:
        z_flags = "full|dataonly";
        break;
      default:
        assert(0);
    }

    __tvfs_exec_tcl(p, "xSync", 
        Tcl_NewStringObj(fd->filename, -1), fd->shm_id,
        Tcl_NewStringObj(z_flags, -1), 0
    );
    __tvfs_result_code(p, &rc);
  }

  if( rc==DBSQL_OK && __tvfs_inject_fullerr(p) ) rc = DBSQL_FULL;

  if( rc==DBSQL_OK ){
    rc = __dbsql_os_sync(fd->real, flags);
  }

  return rc;
}

/*
** Return the current file-size of an tvfs-file.
*/
static int __tvfs_file_size(dbsql_file_t *file, int64_t *size){
  testvfs_fd_t *p = TVFS_GET_FD(file);
  return __dbsql_os_file_size(p->real, size);
}

/*
** Lock an tvfs-file.
*/
static int __tvfs_lock(dbsql_file_t *file, int lock){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->script && p->mask&TESTVFS_LOCK_MASK ){
    char z_lock[30];
    dbsql_snprintf(sizeof(z_lock),z_lock,"%d",lock);
    __tvfs_exec_tcl(p, "xLock", Tcl_NewStringObj(fd->filename, -1), 
                   Tcl_NewStringObj(z_lock, -1), 0, 0);
  }
  if( p->mask&TESTVFS_LOCK_MASK && __tvfs_inject_ioerr(p) ){
    return DBSQL_IOERR_LOCK;
  }
  return __dbsql_os_lock(fd->real, lock);
}

/*
** Unlock an tvfs-file.
*/
static int __tvfs_unlock(dbsql_file_t *file, int lock){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->script && p->mask&TESTVFS_UNLOCK_MASK ){
    char z_lock[30];
    dbsql_snprintf(sizeof(z_lock),z_lock,"%d",lock);
    __tvfs_exec_tcl(p, "xUnlock", Tcl_NewStringObj(fd->filename, -1), 
                   Tcl_NewStringObj(z_lock, -1), 0, 0);
  }
  if( p->mask&TESTVFS_UNLOCK_MASK && __tvfs_inject_ioerr(p) ){
    return DBSQL_IOERR_UNLOCK;
  }
  return __dbsql_os_unlock(fd->real, lock);
}

/*
** Check if another file-handle holds a RESERVED lock on an tvfs-file.
*/
static int __tvfs_check_reserved_lock(dbsql_file_t *file, int *res_out){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->script && p->mask&TESTVFS_CKLOCK_MASK ){
    __tvfs_exec_tcl(p, "xCheckReservedLock", Tcl_NewStringObj(fd->filename, -1),
                   0, 0, 0);
  }
  return __dbsql_os_check_reserved_lock(fd->real, res_out);
}

/*
** File control method. For custom operations on an tvfs-file.
*/
static int __tvfs_file_control(dbsql_file_t *file, int op, void *arg){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( op==DBSQL_FCNTL_PRAGMA ){
    char **argv = (char**)arg;
    if( dbsql_stricmp(argv[1],"error")==0 ){
      int rc = DBSQL_ERROR;
      if( argv[2] ){
        const char *z = argv[2];
        int x = atoi(z);
        if( x ){
          rc = x;
          while( __isdigit(z[0]) ){ z++; }
          while( __isspace(z[0]) ){ z++; }
        }
        if( z[0] ) argv[0] = dbsql_mprintf("%s", z);
      }
      return rc;
    }
    if( dbsql_stricmp(argv[1], "filename")==0 ){
      argv[0] = dbsql_mprintf("%s", fd->filename);
      return DBSQL_OK;
    }
  }
  if( p->script && (p->mask&TESTVFS_FCNTL_MASK) ){
    struct fcntl_t {
      int i_fnctl;
      const char *fnctl;
    } fs[] = {
      { DBSQL_FCNTL_BEGIN_ATOMIC_WRITE, "BEGIN_ATOMIC_WRITE" },
      { DBSQL_FCNTL_COMMIT_ATOMIC_WRITE, "COMMIT_ATOMIC_WRITE" },
      { DBSQL_FCNTL_ZIPVFS, "ZIPVFS" },
    };
    int i;
    for(i=0; i<sizeof(fs)/sizeof(fs[0]); i++){
      if( op==fs[i].i_fnctl ) break;
    }
    if( i<sizeof(fs)/sizeof(fs[0]) ){
      int rc = 0;
      __tvfs_exec_tcl(p, "xFileControl", 
          Tcl_NewStringObj(fd->filename, -1), 
          Tcl_NewStringObj(fs[i].fnctl, -1),
          0, 0
      );
      __tvfs_result_code(p, &rc);
      if( rc ) return (rc<0 ? DBSQL_OK : rc);
    }
  }
  return __dbsql_os_file_control(fd->real, op, arg);
}

/*
** Return the sector-size in bytes for an tvfs-file.
*/
static int __tvfs_sector_size(dbsql_file_t *file){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->i_sectorsize>=0 ){
    return p->i_sectorsize;
  }
  return __dbsql_os_sector_size(fd->real);
}

/*
** Return the device characteristic flags supported by an tvfs-file.
*/
static int __tvfs_device_characteristics(dbsql_file_t *file){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)fd->vfs->app_data;
  if( p->i_devchar>=0 ){
    return p->i_devchar;
  }
  return __dbsql_os_device_characteristics(fd->real);
}

/*
** Open an tvfs file handle.
*/
static int __tvfs_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  int rc;
  testvfs_file_t *testfile = (testvfs_file_t *)file;
  testvfs_fd_t *fd;
  Tcl_Obj *id = 0;
  testvfs_t *p = (testvfs_t *)vfs->app_data;

  fd = (testvfs_fd_t *)ckalloc(sizeof(testvfs_fd_t) + PARENTVFS(vfs)->sz_os_file);
  memset(fd, 0, sizeof(testvfs_fd_t) + PARENTVFS(vfs)->sz_os_file);
  fd->shm = 0;
  fd->shm_id = 0;
  fd->filename = name;
  fd->vfs = vfs;
  fd->real = (dbsql_file_t *)&fd[1];
  memset(testfile, 0, sizeof(testvfs_file_t));
  testfile->fd = fd;

  /* Evaluate the Tcl script: 
  **
  **   SCRIPT x_open FILENAME KEY-VALUE-ARGS
  **
  ** If the script returns an dbsql error code other than DBSQL_OK, an
  ** error is returned to the caller. If it returns DBSQL_OK, the new
  ** connection is named "anon". Otherwise, the value returned by the
  ** script is used as the connection name.
  */
  Tcl_ResetResult(p->interp);
  if( p->script && p->mask&TESTVFS_OPEN_MASK ){
    Tcl_Obj *arg = Tcl_NewObj();
    Tcl_IncrRefCount(arg);
    if( flags&DBSQL_OPEN_MAIN_DB ){
      const char *z = &name[strlen(name)+1];
      while( *z ){
        Tcl_ListObjAppendElement(0, arg, Tcl_NewStringObj(z, -1));
        z += strlen(z) + 1;
        Tcl_ListObjAppendElement(0, arg, Tcl_NewStringObj(z, -1));
        z += strlen(z) + 1;
      }
    }
    __tvfs_exec_tcl(p, "xOpen", Tcl_NewStringObj(fd->filename, -1), arg, 0, 0);
    Tcl_DecrRefCount(arg);
    if( __tvfs_result_code(p, &rc) ){
      if( rc!=DBSQL_OK ) return rc;
    }else{
      id = Tcl_GetObjResult(p->interp);
    }
  }

  if( (p->mask&TESTVFS_OPEN_MASK) &&  __tvfs_inject_ioerr(p) ) return DBSQL_IOERR;
  if( __tvfs_inject_cantopenerr(p) ) return DBSQL_CANTOPEN;
  if( __tvfs_inject_fullerr(p) ) return DBSQL_FULL;

  if( !id ){
    id = Tcl_NewStringObj("anon", -1);
  }
  Tcl_IncrRefCount(id);
  fd->shm_id = id;
  Tcl_ResetResult(p->interp);

  rc = __dbsql_os_open(PARENTVFS(vfs), name, fd->real, flags, out_flags);
  if( fd->real->methods ){
    dbsql_io_methods_t *methods;
    int n_byte;

    if( vfs->i_version>1 ){
      n_byte = sizeof(dbsql_io_methods_t);
    }else{
      n_byte = offsetof(dbsql_io_methods_t, shm_map);
    }

    methods = (dbsql_io_methods_t *)ckalloc(n_byte);
    memcpy(methods, &tvfs_io_methods, n_byte);
    methods->i_version = fd->real->methods->i_version;
    if( methods->i_version>vfs->i_version ){
      methods->i_version = vfs->i_version;
    }
    if( vfs->i_version>1 && ((testvfs_t *)vfs->app_data)->is_noshm ){
      methods->shm_unmap = 0;
      methods->shm_lock = 0;
      methods->shm_barrier = 0;
      methods->shm_map = 0;
    }
    file->methods = methods;
  }

  return rc;
}

/*
** Delete the file located at path. If the dir_sync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int __tvfs_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  int rc = DBSQL_OK;
  testvfs_t *p = (testvfs_t *)vfs->app_data;

  if( p->script && p->mask&TESTVFS_DELETE_MASK ){
    __tvfs_exec_tcl(p, "xDelete", 
        Tcl_NewStringObj(path, -1), Tcl_NewIntObj(dir_sync), 0, 0
    );
    __tvfs_result_code(p, &rc);
  }
  if( rc==DBSQL_OK ){
    rc = __dbsql_os_delete(PARENTVFS(vfs), path, dir_sync);
  }
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int __tvfs_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  testvfs_t *p = (testvfs_t *)vfs->app_data;
  if( p->script && p->mask&TESTVFS_ACCESS_MASK ){
    int rc;
    char *arg = 0;
    if( flags==DBSQL_ACCESS_EXISTS ) arg = "DBSQL_ACCESS_EXISTS";
    if( flags==DBSQL_ACCESS_READWRITE ) arg = "DBSQL_ACCESS_READWRITE";
    if( flags==DBSQL_ACCESS_READ ) arg = "DBSQL_ACCESS_READ";
    __tvfs_exec_tcl(p, "xAccess", 
        Tcl_NewStringObj(path, -1), Tcl_NewStringObj(arg, -1), 0, 0
    );
    if( __tvfs_result_code(p, &rc) ){
      if( rc!=DBSQL_OK ) return rc;
    }else{
      Tcl_Interp *interp = p->interp;
      if( TCL_OK==Tcl_GetBooleanFromObj(0, Tcl_GetObjResult(interp), res_out) ){
        return DBSQL_OK;
      }
    }
  }
  return __dbsql_os_access(PARENTVFS(vfs), path, flags, res_out);
}

/*
** Populate buffer out with the full canonical pathname corresponding
** to the pathname in path. out is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int __tvfs_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  testvfs_t *p = (testvfs_t *)vfs->app_data;
  if( p->script && p->mask&TESTVFS_FULLPATHNAME_MASK ){
    int rc;
    __tvfs_exec_tcl(p, "xFullPathname", Tcl_NewStringObj(path, -1), 0, 0, 0);
    if( __tvfs_result_code(p, &rc) ){
      if( rc!=DBSQL_OK ) return rc;
    }
  }
  return __dbsql_os_full_pathname(PARENTVFS(vfs), path, n_out, out);
}

#ifndef DBSQL_OMIT_LOAD_EXTENSION
/*
** Open the dynamic library located at path and return a handle.
*/
static void *__tvfs_dl_open(dbsql_vfs_t *vfs, const char *path){
  return __dbsql_os_dl_open(PARENTVFS(vfs), path);
}

/*
** Populate the buffer errmsg (size n_byte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void __tvfs_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  __dbsql_os_dl_error(PARENTVFS(vfs), n_byte, errmsg);
}

/*
** Return a pointer to the symbol symbol in the dynamic library handle.
*/
static void (*tvfs_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return __dbsql_os_dl_sym(PARENTVFS(vfs), p, sym);
}

/*
** Close the dynamic library handle handle.
*/
static void __tvfs_dl_close(dbsql_vfs_t *vfs, void *handle){
  __dbsql_os_dl_close(PARENTVFS(vfs), handle);
}
#endif /* DBSQL_OMIT_LOAD_EXTENSION */

/*
** Populate the buffer pointed to by buf_out with n_byte bytes of 
** random data.
*/
static int __tvfs_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return __dbsql_os_randomness(PARENTVFS(vfs), n_byte, buf_out);
}

/*
** Sleep for n_micro microseconds. Return the number of microseconds 
** actually slept.
*/
static int __tvfs_sleep(dbsql_vfs_t *vfs, int n_micro){
  testvfs_t *p = (testvfs_t *)vfs->app_data;
  if( p->script && (p->mask&TESTVFS_SLEEP_MASK) ){
    __tvfs_exec_tcl(p, "xSleep", Tcl_NewIntObj(n_micro), 0, 0, 0);
  }
  return __dbsql_os_sleep(PARENTVFS(vfs), n_micro);
}

/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __tvfs_current_time(dbsql_vfs_t *vfs, double *time_out){
  return PARENTVFS(vfs)->current_time(PARENTVFS(vfs), time_out);
}

static int __tvfs_shm_open(dbsql_file_t *file){
  testvfs_t *p;
  int rc = DBSQL_OK;             /* Return code */
  testvfs_buffer_t *buffer;         /* Buffer to open connection to */
  testvfs_fd_t *fd;                 /* The testvfs file structure */

  fd = TVFS_GET_FD(file);
  p = (testvfs_t *)fd->vfs->app_data;
  assert( 0==p->is_fullshm );
  assert( fd->shm_id && fd->shm==0 && fd->next==0 );

  /* Evaluate the Tcl script: 
  **
  **   SCRIPT shm_open FILENAME
  */
  Tcl_ResetResult(p->interp);
  if( p->script && p->mask&TESTVFS_SHMOPEN_MASK ){
    __tvfs_exec_tcl(p, "xShmOpen", Tcl_NewStringObj(fd->filename, -1), 0, 0, 0);
    if( __tvfs_result_code(p, &rc) ){
      if( rc!=DBSQL_OK ) return rc;
    }
  }

  assert( rc==DBSQL_OK );
  if( p->mask&TESTVFS_SHMOPEN_MASK && __tvfs_inject_ioerr(p) ){
    return DBSQL_IOERR;
  }

  /* Search for a testvfs_buffer_t. Create a new one if required. */
  for(buffer=p->buffer; buffer; buffer=buffer->next){
    if( 0==strcmp(fd->filename, buffer->z_file) ) break;
  }
  if( !buffer ){
    int sz_name = (int)strlen(fd->filename);
    int n_byte = sizeof(testvfs_buffer_t) + sz_name + 1;
    buffer = (testvfs_buffer_t *)ckalloc(n_byte);
    memset(buffer, 0, n_byte);
    buffer->z_file = (char *)&buffer[1];
    memcpy(buffer->z_file, fd->filename, sz_name+1);
    buffer->next = p->buffer;
    p->buffer = buffer;
  }

  /* Connect the testvfs_buffer_t to the new testvfs_shm_t handle and return. */
  fd->next = buffer->file;
  buffer->file = fd;
  fd->shm = buffer;
  return rc;
}

static void __tvfs_alloc_page(testvfs_buffer_t *p, int i_page, int pgsz){
  assert( i_page<TESTVFS_MAX_PAGES );
  if( p->pages[i_page]==0 ){
    p->pages[i_page] = (u_int8_t *)ckalloc(pgsz);
    memset(p->pages[i_page], 0, pgsz);
    p->pgsz = pgsz;
  }
}

static int __tvfs_shm_map(
  dbsql_file_t *file,            /* Handle open on database file */
  int i_page,                      /* Page to retrieve */
  int pgsz,                       /* Size of pages */
  int is_write,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)(fd->vfs->app_data);

  if( p->is_fullshm ){
    dbsql_file_t *real = fd->real;
    return real->methods->shm_map(real, i_page, pgsz, is_write, pp);
  }

  if( 0==fd->shm ){
    rc = __tvfs_shm_open(file);
    if( rc!=DBSQL_OK ){
      return rc;
    }
  }

  if( p->script && p->mask&TESTVFS_SHMMAP_MASK ){
    Tcl_Obj *arg = Tcl_NewObj();
    Tcl_IncrRefCount(arg);
    Tcl_ListObjAppendElement(p->interp, arg, Tcl_NewIntObj(i_page));
    Tcl_ListObjAppendElement(p->interp, arg, Tcl_NewIntObj(pgsz));
    Tcl_ListObjAppendElement(p->interp, arg, Tcl_NewIntObj(is_write));
    __tvfs_exec_tcl(p, "xShmMap", 
        Tcl_NewStringObj(fd->shm->z_file, -1), fd->shm_id, arg, 0
    );
    __tvfs_result_code(p, &rc);
    Tcl_DecrRefCount(arg);
  }
  if( rc==DBSQL_OK && p->mask&TESTVFS_SHMMAP_MASK && __tvfs_inject_ioerr(p) ){
    rc = DBSQL_IOERR;
  }

  if( rc==DBSQL_OK && is_write && !fd->shm->pages[i_page] ){
    __tvfs_alloc_page(fd->shm, i_page, pgsz);
  }
  if( rc==DBSQL_OK || rc==DBSQL_READONLY ){
    *pp = (void volatile *)fd->shm->pages[i_page];
  }

  return rc;
}


static int __tvfs_shm_lock(
  dbsql_file_t *file,
  int ofst,
  int n,
  int flags
){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)(fd->vfs->app_data);
  int n_lock;
  char lock[80];

  if( p->is_fullshm ){
    dbsql_file_t *real = fd->real;
    return real->methods->shm_lock(real, ofst, n, flags);
  }

  if( p->script && p->mask&TESTVFS_SHMLOCK_MASK ){
    dbsql_snprintf(sizeof(lock), lock, "%d %d", ofst, n);
    n_lock = (int)strlen(lock);
    if( flags & DBSQL_SHM_LOCK ){
      strcpy(&lock[n_lock], " lock");
    }else{
      strcpy(&lock[n_lock], " unlock");
    }
    n_lock += (int)strlen(&lock[n_lock]);
    if( flags & DBSQL_SHM_SHARED ){
      strcpy(&lock[n_lock], " shared");
    }else{
      strcpy(&lock[n_lock], " exclusive");
    }
    __tvfs_exec_tcl(p, "xShmLock", 
        Tcl_NewStringObj(fd->shm->z_file, -1), fd->shm_id,
        Tcl_NewStringObj(lock, -1), 0
    );
    __tvfs_result_code(p, &rc);
  }

  if( rc==DBSQL_OK && p->mask&TESTVFS_SHMLOCK_MASK && __tvfs_inject_ioerr(p) ){
    rc = DBSQL_IOERR;
  }

  if( rc==DBSQL_OK ){
    int is_lock = (flags & DBSQL_SHM_LOCK);
    int is_excl = (flags & DBSQL_SHM_EXCLUSIVE);
    u_int32_t mask = (((1<<n)-1) << ofst);
    if( is_lock ){
      testvfs_fd_t *p2;
      for(p2=fd->shm->file; p2; p2=p2->next){
        if( p2==fd ) continue;
        if( (p2->excllock&mask) || (is_excl && p2->sharedlock&mask) ){
          rc = DBSQL_BUSY;
          break;
        }
      }
      if( rc==DBSQL_OK ){
        if( is_excl )  fd->excllock |= mask;
        if( !is_excl ) fd->sharedlock |= mask;
      }
    }else{
      if( is_excl )  fd->excllock &= (~mask);
      if( !is_excl ) fd->sharedlock &= (~mask);
    }
  }

  return rc;
}

static void __tvfs_shm_barrier(dbsql_file_t *file){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)(fd->vfs->app_data);

  if( p->script && p->mask&TESTVFS_SHMBARRIER_MASK ){
    const char *z = fd->shm ? fd->shm->z_file : "";
    __tvfs_exec_tcl(p, "xShmBarrier", Tcl_NewStringObj(z, -1), fd->shm_id, 0, 0);
  }

  if( p->is_fullshm ){
    dbsql_file_t *real = fd->real;
    real->methods->shm_barrier(real);
    return;
  }
}

static int __tvfs_shm_unmap(
  dbsql_file_t *file,
  int delete_flag
){
  int rc = DBSQL_OK;
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  testvfs_t *p = (testvfs_t *)(fd->vfs->app_data);
  testvfs_buffer_t *buffer = fd->shm;
  testvfs_fd_t **pp_fd;

  if( p->is_fullshm ){
    dbsql_file_t *real = fd->real;
    return real->methods->shm_unmap(real, delete_flag);
  }

  if( !buffer ) return DBSQL_OK;
  assert( fd->shm_id && fd->shm );

  if( p->script && p->mask&TESTVFS_SHMCLOSE_MASK ){
    __tvfs_exec_tcl(p, "xShmUnmap", 
        Tcl_NewStringObj(fd->shm->z_file, -1), fd->shm_id, 0, 0
    );
    __tvfs_result_code(p, &rc);
  }

  for(pp_fd=&buffer->file; *pp_fd!=fd; pp_fd=&((*pp_fd)->next));
  assert( (*pp_fd)==fd );
  *pp_fd = fd->next;
  fd->next = 0;

  if( buffer->file==0 ){
    int i;
    testvfs_buffer_t **pp;
    for(pp=&p->buffer; *pp!=buffer; pp=&((*pp)->next));
    *pp = (*pp)->next;
    for(i=0; buffer->pages[i]; i++){
      ckfree((char *)buffer->pages[i]);
    }
    ckfree((char *)buffer);
  }
  fd->shm = 0;

  return rc;
}

static int __tvfs_fetch(
    dbsql_file_t *file, 
    int64_t i_ofst, 
    int i_amt, 
    void **pp
){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  return __dbsql_os_fetch(fd->real, i_ofst, i_amt, pp);
}

static int __tvfs_unfetch(dbsql_file_t *file, int64_t i_ofst, void *p){
  testvfs_fd_t *fd = TVFS_GET_FD(file);
  return __dbsql_os_unfetch(fd->real, i_ofst, p);
}

static int DBSQL_TCLAPI testvfs_obj_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  testvfs_t *p = (testvfs_t *)cd;

  enum DB_enum { 
    CMD_SHM, CMD_DELETE, CMD_FILTER, CMD_IOERR, CMD_SCRIPT, 
    CMD_DEVCHAR, CMD_SECTORSIZE, CMD_FULLERR, CMD_CANTOPENERR
  };
  struct testvfs_subcmd_t {
    char *name;
    enum DB_enum cmd;
  } subcmds[] = {
    { "shm",         CMD_SHM         },
    { "delete",      CMD_DELETE      },
    { "filter",      CMD_FILTER      },
    { "ioerr",       CMD_IOERR       },
    { "fullerr",     CMD_FULLERR     },
    { "cantopenerr", CMD_CANTOPENERR },
    { "script",      CMD_SCRIPT      },
    { "devchar",     CMD_DEVCHAR     },
    { "sectorsize",  CMD_SECTORSIZE  },
    { 0, 0 }
  };
  int i;
  
  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObjStruct(
        interp, objv[1], subcmds, sizeof(subcmds[0]), "subcommand", 0, &i) 
  ){
    return TCL_ERROR;
  }
  Tcl_ResetResult(interp);

  switch( subcmds[i].cmd ){
    case CMD_SHM: {
      Tcl_Obj *obj;
      int rc;
      testvfs_buffer_t *buffer;
      char *name;
      if( objc!=3 && objc!=4 ){
        Tcl_WrongNumArgs(interp, 2, objv, "FILE ?VALUE?");
        return TCL_ERROR;
      }
      name = ckalloc(p->parent->mx_pathname);
      rc = p->parent->full_pathname(
          p->parent, Tcl_GetString(objv[2]), 
          p->parent->mx_pathname, name
      );
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, "failed to get full path: ",
                         Tcl_GetString(objv[2]), NULL);
        ckfree(name);
        return TCL_ERROR;
      }
      for(buffer=p->buffer; buffer; buffer=buffer->next){
        if( 0==strcmp(buffer->z_file, name) ) break;
      }
      ckfree(name);
      if( !buffer ){
        Tcl_AppendResult(interp, "no such file: ", Tcl_GetString(objv[2]), NULL);
        return TCL_ERROR;
      }
      if( objc==4 ){
        TCL_SIZE n;
        u_int8_t *a = Tcl_GetByteArrayFromObj(objv[3], &n);
        int pgsz = buffer->pgsz;
        if( pgsz==0 ) pgsz = 65536;
        for(i=0; i*pgsz<(int)n; i++){
          int n_byte = pgsz;
          __tvfs_alloc_page(buffer, i, pgsz);
          if( n-i*pgsz<pgsz ){
            n_byte = (int)n;
          }
          memcpy(buffer->pages[i], &a[i*pgsz], n_byte);
        }
      }

      obj = Tcl_NewObj();
      for(i=0; buffer->pages[i]; i++){
        int pgsz = buffer->pgsz;
        if( pgsz==0 ) pgsz = 65536;
        Tcl_AppendObjToObj(obj, Tcl_NewByteArrayObj(buffer->pages[i], pgsz));
      }
      Tcl_SetObjResult(interp, obj);
      break;
    }

    /*  TESTVFS filter METHOD-LIST
    **
    **     Activate special processing for those methods contained in the list
    */
    case CMD_FILTER: {
      static struct vfs_method_t {
        char *name;
        int mask;
      } vfsmethod [] = {
        { "xShmOpen",           TESTVFS_SHMOPEN_MASK },
        { "xShmLock",           TESTVFS_SHMLOCK_MASK },
        { "xShmBarrier",        TESTVFS_SHMBARRIER_MASK },
        { "xShmUnmap",          TESTVFS_SHMCLOSE_MASK },
        { "xShmMap",            TESTVFS_SHMMAP_MASK },
        { "xSync",              TESTVFS_SYNC_MASK },
        { "xDelete",            TESTVFS_DELETE_MASK },
        { "xWrite",             TESTVFS_WRITE_MASK },
        { "xRead",              TESTVFS_READ_MASK },
        { "xTruncate",          TESTVFS_TRUNCATE_MASK },
        { "xOpen",              TESTVFS_OPEN_MASK },
        { "xClose",             TESTVFS_CLOSE_MASK },
        { "xAccess",            TESTVFS_ACCESS_MASK },
        { "xFullPathname",      TESTVFS_FULLPATHNAME_MASK },
        { "xUnlock",            TESTVFS_UNLOCK_MASK },
        { "xLock",              TESTVFS_LOCK_MASK },
        { "xCheckReservedLock", TESTVFS_CKLOCK_MASK },
        { "xFileControl",       TESTVFS_FCNTL_MASK },
        { "xSleep",             TESTVFS_SLEEP_MASK },
      };
      Tcl_Obj **elems = 0;
      TCL_SIZE n_elem = 0;
      int mask = 0;
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "LIST");
        return TCL_ERROR;
      }
      if( Tcl_ListObjGetElements(interp, objv[2], &n_elem, &elems) ){
        return TCL_ERROR;
      }
      Tcl_ResetResult(interp);
      for(i=0; i<(int)n_elem; i++){
        int i_method;
        char *elem = Tcl_GetString(elems[i]);
        for(i_method=0; i_method<ARRAY_SIZE(vfsmethod); i_method++){
          if( strcmp(elem, vfsmethod[i_method].name)==0 ){
            mask |= vfsmethod[i_method].mask;
            break;
          }
        }
        if( i_method==ARRAY_SIZE(vfsmethod) ){
          Tcl_AppendResult(interp, "unknown method: ", elem, NULL);
          return TCL_ERROR;
        }
      }
      p->mask = mask;
      break;
    }

    /*
    **  TESTVFS script ?SCRIPT?
    **
    **  Query or set the script to be run when filtered VFS events
    **  occur.
    */
    case CMD_SCRIPT: {
      if( objc==3 ){
        TCL_SIZE n_byte;
        if( p->script ){
          Tcl_DecrRefCount(p->script);
          p->script = 0;
        }
        Tcl_GetStringFromObj(objv[2], &n_byte);
        if( n_byte>0 ){
          p->script = Tcl_DuplicateObj(objv[2]);
          Tcl_IncrRefCount(p->script);
        }
      }else if( objc!=2 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?SCRIPT?");
        return TCL_ERROR;
      }

      Tcl_ResetResult(interp);
      if( p->script ) Tcl_SetObjResult(interp, p->script);

      break;
    }

    /*
    ** TESTVFS ioerr ?IFAIL PERSIST?
    **
    **   Where IFAIL is an integer and PERSIST is boolean.
    */
    case CMD_CANTOPENERR:
    case CMD_IOERR:
    case CMD_FULLERR: {
      test_fault_inject_t *test = 0;
      int i_ret;

      switch( subcmds[i].cmd ){
        case CMD_IOERR: test = &p->ioerr_err; break;
        case CMD_FULLERR: test = &p->full_err; break;
        case CMD_CANTOPENERR: test = &p->cantopen_err; break;
        default: assert(0);
      }
      i_ret = test->n_fail;
      test->n_fail = 0;
      test->fault = 0;
      test->i_cnt = 0;

      if( objc==4 ){
        int i_cnt, i_persist;
        if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &i_cnt)
         || TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[3], &i_persist)
        ){
          return TCL_ERROR;
        }
        test->fault = i_persist?FAULT_INJECT_PERSISTENT:FAULT_INJECT_TRANSIENT;
        test->i_cnt = i_cnt;
      }else if( objc!=2 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?CNT PERSIST?");
        return TCL_ERROR;
      }
      Tcl_SetObjResult(interp, Tcl_NewIntObj(i_ret));
      break;
    }

    case CMD_DELETE: {
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;
    }

    case CMD_DEVCHAR: {
      struct device_flag_t {
        char *name;
        int i_value;
      } a_flag[] = {
        { "default",               -1 },
        { "atomic",                DBSQL_IOCAP_ATOMIC                },
        { "atomic512",             DBSQL_IOCAP_ATOMIC512             },
        { "atomic1k",              DBSQL_IOCAP_ATOMIC1K              },
        { "atomic2k",              DBSQL_IOCAP_ATOMIC2K              },
        { "atomic4k",              DBSQL_IOCAP_ATOMIC4K              },
        { "atomic8k",              DBSQL_IOCAP_ATOMIC8K              },
        { "atomic16k",             DBSQL_IOCAP_ATOMIC16K             },
        { "atomic32k",             DBSQL_IOCAP_ATOMIC32K             },
        { "atomic64k",             DBSQL_IOCAP_ATOMIC64K             },
        { "sequential",            DBSQL_IOCAP_SEQUENTIAL            },
        { "safe_append",           DBSQL_IOCAP_SAFE_APPEND           },
        { "undeletable_when_open", DBSQL_IOCAP_UNDELETABLE_WHEN_OPEN },
        { "powersafe_overwrite",   DBSQL_IOCAP_POWERSAFE_OVERWRITE   },
        { "immutable",             DBSQL_IOCAP_IMMUTABLE             },
        { 0, 0 }
      };
      Tcl_Obj *ret;
      int i_flag;

      if( objc>3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?ATTR-LIST?");
        return TCL_ERROR;
      }
      if( objc==3 ){
        int j;
        int i_new = 0;
        Tcl_Obj **flags = 0;
        TCL_SIZE n_flags = 0;

        if( Tcl_ListObjGetElements(interp, objv[2], &n_flags, &flags) ){
          return TCL_ERROR;
        }

        for(j=0; j<(int)n_flags; j++){
          int idx = 0;
          if( Tcl_GetIndexFromObjStruct(interp, flags[j], a_flag, 
                sizeof(a_flag[0]), "flag", 0, &idx) 
          ){
            return TCL_ERROR;
          }
          if( a_flag[idx].i_value<0 && n_flags>1 ){
            Tcl_AppendResult(interp, "bad flags: ", Tcl_GetString(objv[2]), NULL);
            return TCL_ERROR;
          }
          i_new |= a_flag[idx].i_value;
        }

        p->i_devchar = i_new| 0x10000000;
      }

      ret = Tcl_NewObj();
      for(i_flag=0; i_flag<sizeof(a_flag)/sizeof(a_flag[0]); i_flag++){
        if( p->i_devchar & a_flag[i_flag].i_value ){
          Tcl_ListObjAppendElement(
              interp, ret, Tcl_NewStringObj(a_flag[i_flag].name, -1)
          );
        }
      }
      Tcl_SetObjResult(interp, ret);

      break;
    }

    case CMD_SECTORSIZE: {
      if( objc>3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?VALUE?");
        return TCL_ERROR;
      }
      if( objc==3 ){
        int i_new = 0;
        if( Tcl_GetIntFromObj(interp, objv[2], &i_new) ){
          return TCL_ERROR;
        }
        p->i_sectorsize = i_new;
      }
      Tcl_SetObjResult(interp, Tcl_NewIntObj(p->i_sectorsize));
      break;
    }
  }

  return TCL_OK;
}

static void DBSQL_TCLAPI testvfs_obj_del(ClientData cd){
  testvfs_t *p = (testvfs_t *)cd;
  if( p->script ) Tcl_DecrRefCount(p->script);
  dbsql_vfs_unregister(p->vfs);
  memset(p->vfs, 0, sizeof(dbsql_vfs_t));
  ckfree((char *)p->vfs);
  memset(p, 0, sizeof(testvfs_t));
  ckfree((char *)p);
}

/*
** Usage:  testvfs VFSNAME ?SWITCHES?
**
** Switches are:
**
**   -noshm   BOOLEAN             (True to omit shm methods. Default false)
**   -default BOOLEAN             (True to make the vfs default. Default false)
**
** This command creates two things when it is invoked: an dbsql VFS, and
** a Tcl command. Both are named VFSNAME. The VFS is installed. It is not
** installed as the default VFS.
**
** The VFS passes all file I/O calls through to the underlying VFS.
**
** Whenever the shm_map method of the VFS
** is invoked, the SCRIPT is executed as follows:
**
**   SCRIPT shm_map    FILENAME ID
**
** The value returned by the invocation of SCRIPT above is interpreted as
** an dbsql error code and returned to dbsql. Either a symbolic 
** "DBSQL_OK" or numeric "0" value may be returned.
**
** The contents of the shared-memory buffer associated with a given file
** may be read and set using the following command:
**
**   VFSNAME shm FILENAME ?NEWVALUE?
**
** When the shm_lock method is invoked by dbsql, the following script is
** run:
**
**   SCRIPT shm_lock    FILENAME ID LOCK
**
** where LOCK is of the form "OFFSET NBYTE lock/unlock shared/exclusive"
*/
static int DBSQL_TCLAPI testvfs_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static dbsql_vfs_t tvfs_vfs = {
    3,                            /* i_version */
    0,                            /* sz_os_file */
    0,                            /* mx_pathname */
    0,                            /* next */
    0,                            /* name */
    0,                            /* app_data */
    __tvfs_open,                     /* x_open */
    __tvfs_delete,                   /* x_delete */
    __tvfs_access,                   /* x_access */
    __tvfs_full_pathname,             /* full_pathname */
#ifndef DBSQL_OMIT_LOAD_EXTENSION
    __tvfs_dl_open,                   /* dl_open */
    __tvfs_dl_error,                  /* dl_error */
    tvfs_dl_sym,                    /* dl_sym */
    __tvfs_dl_close,                  /* dl_close */
#else
    0,                            /* dl_open */
    0,                            /* dl_error */
    0,                            /* dl_sym */
    0,                            /* dl_close */
#endif /* DBSQL_OMIT_LOAD_EXTENSION */
    __tvfs_randomness,               /* randomness */
    __tvfs_sleep,                    /* x_sleep */
    __tvfs_current_time,              /* current_time */
    0,                            /* get_last_error */
    0,                            /* current_time_int64 */
    0,                            /* set_system_call */
    0,                            /* get_system_call */
    0,                            /* next_system_call */
  };

  testvfs_t *p;                     /* New object */
  dbsql_vfs_t *vfs;              /* New VFS */
  char *z_vfs;
  int n_byte;                      /* Bytes of space to allocate at p */

  int i;
  int is_noshm = 0;                /* True if -noshm is passed */
  int is_fullshm = 0;              /* True if -fullshm is passed */
  int is_default = 0;              /* True if -default is passed */
  int sz_os_file = 0;               /* Value passed to -szosfile */
  int mx_pathname = -1;            /* Value passed to -mxpathname */
  int i_version = 3;               /* Value passed to -iversion */

  if( objc<2 || 0!=(objc%2) ) goto bad_args;
  for(i=2; i<objc; i += 2){
    TCL_SIZE n_switch;
    char *z_switch;
    z_switch = Tcl_GetStringFromObj(objv[i], &n_switch); 

    if( n_switch>2 && 0==strncmp("-noshm", z_switch, n_switch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &is_noshm) ){
        return TCL_ERROR;
      }
      if( is_noshm ) is_fullshm = 0;
    }
    else if( n_switch>2 && 0==strncmp("-default", z_switch, n_switch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &is_default) ){
        return TCL_ERROR;
      }
    }
    else if( n_switch>2 && 0==strncmp("-szosfile", z_switch, n_switch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &sz_os_file) ){
        return TCL_ERROR;
      }
    }
    else if( n_switch>2 && 0==strncmp("-mxpathname", z_switch, n_switch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &mx_pathname) ){
        return TCL_ERROR;
      }
    }
    else if( n_switch>2 && 0==strncmp("-iversion", z_switch, n_switch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &i_version) ){
        return TCL_ERROR;
      }
    }
    else if( n_switch>2 && 0==strncmp("-fullshm", z_switch, n_switch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &is_fullshm) ){
        return TCL_ERROR;
      }
      if( is_fullshm ) is_noshm = 0;
    }
    else{
      goto bad_args;
    }
  }

  if( sz_os_file<sizeof(testvfs_file_t) ){
    sz_os_file = sizeof(testvfs_file_t);
  }

  z_vfs = Tcl_GetString(objv[1]);
  n_byte = sizeof(testvfs_t) + (int)strlen(z_vfs)+1;
  p = (testvfs_t *)ckalloc(n_byte);
  memset(p, 0, n_byte);
  p->i_devchar = -1;
  p->i_sectorsize = -1;

  /* Create the new object command before querying dbsql for a default VFS
  ** to use for 'real' IO operations. This is because creating the new VFS
  ** may delete an existing [testvfs] VFS of the same name. If such a VFS
  ** is currently the default, the new [testvfs] may end up calling the 
  ** methods of a deleted object.
  */
  Tcl_CreateObjCommand(interp, z_vfs, testvfs_obj_cmd, p, testvfs_obj_del);
  p->parent = dbsql_vfs_find(0);
  p->interp = interp;

  p->name = (char *)&p[1];
  memcpy(p->name, z_vfs, strlen(z_vfs)+1);

  vfs = (dbsql_vfs_t *)ckalloc(sizeof(dbsql_vfs_t));
  memcpy(vfs, &tvfs_vfs, sizeof(dbsql_vfs_t));
  vfs->app_data = (void *)p;
  vfs->i_version = i_version;
  vfs->name = p->name;
  vfs->mx_pathname = p->parent->mx_pathname;
  if( mx_pathname>=0 && mx_pathname<vfs->mx_pathname ){
    vfs->mx_pathname = mx_pathname;
  }
  vfs->sz_os_file = sz_os_file;
  p->vfs = vfs;
  p->is_noshm = is_noshm;
  p->is_fullshm = is_fullshm;
  p->mask = TESTVFS_ALL_MASK;

  dbsql_vfs_register(vfs, is_default);

  return TCL_OK;

 bad_args:
  Tcl_WrongNumArgs(interp, 1, objv, "VFSNAME ?-noshm BOOL? ?-fullshm BOOL? ?-default BOOL? ?-mxpathname INT? ?-szosfile INT? ?-iversion INT?");
  return TCL_ERROR;
}

extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);
extern const char *__err_name(int);

/*
** tclcmd: vfs_shmlock DB DBNAME (shared|exclusive) (lock|unlock) OFFSET N
*/
static int DBSQL_TCLAPI test_vfs_shmlock(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *arg1s[] = {"shared", "exclusive", 0};
  const char *arg2s[] = {"lock", "unlock", 0};
  DBSQL *db = 0;
  int rc = DBSQL_OK;
  const char *dbname = 0;
  int i_arg1 = 0;
  int i_arg2 = 0;
  int i_offset = 0;
  int n = 0;
  dbsql_file_t *fd;

  if( objc!=7 ){
    Tcl_WrongNumArgs(interp, 1, objv, 
        "DB DBNAME (shared|exclusive) (lock|unlock) OFFSET N"
    );
    return TCL_ERROR;
  }

  dbname = Tcl_GetString(objv[2]);
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) 
   || Tcl_GetIndexFromObj(interp, objv[3], arg1s, "ARG", 0, &i_arg1) 
   || Tcl_GetIndexFromObj(interp, objv[4], arg2s, "ARG", 0, &i_arg2) 
   || Tcl_GetIntFromObj(interp, objv[5], &i_offset)
   || Tcl_GetIntFromObj(interp, objv[6], &n)
  ){
    return TCL_ERROR;
  }

  dbsql_file_control(db, dbname, DBSQL_FCNTL_FILE_POINTER, (void*)&fd);
  if( fd==0 ){
    return TCL_ERROR;
  }
  rc = fd->methods->shm_lock(fd, i_offset, n, 
      (i_arg1==0 ? DBSQL_SHM_SHARED : DBSQL_SHM_EXCLUSIVE)
    | (i_arg2==0 ? DBSQL_SHM_LOCK : DBSQL_SHM_UNLOCK)
  );
  Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
  return TCL_OK;
}

static int DBSQL_TCLAPI test_vfs_set_readmark(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db = 0;
  int rc = DBSQL_OK;
  const char *dbname = 0;
  int i_slot = 0;
  int i_val = -1;
  dbsql_file_t *fd;
  void volatile *shm = 0;
  u_int32_t *shms;
  int i_off;

  if( objc!=4 && objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB DBNAME SLOT ?VALUE?");
    return TCL_ERROR;
  }

  dbname = Tcl_GetString(objv[2]);
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) 
   || Tcl_GetIntFromObj(interp, objv[3], &i_slot)
   || (objc==5 && Tcl_GetIntFromObj(interp, objv[4], &i_val))
  ){
    return TCL_ERROR;
  }

  dbsql_file_control(db, dbname, DBSQL_FCNTL_FILE_POINTER, (void*)&fd);
  if( fd==0 ){
    return TCL_ERROR;
  }
  rc = fd->methods->shm_map(fd, 0, 32*1024, 0, &shm);
  if( rc!=DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    return TCL_ERROR;
  }
  if( shm==0 ){
    Tcl_AppendResult(interp, "*-shm is not yet mapped", NULL);
    return TCL_ERROR;
  }
  shms = (u_int32_t*)shm;
  i_off = 12*2+1+i_slot;

  if( objc==5 ){
    shms[i_off] = i_val;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(shms[i_off]));

  return TCL_OK;
}

int Dbsqltestvfs_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "testvfs", testvfs_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "vfs_shmlock", test_vfs_shmlock, 0, 0);
  Tcl_CreateObjCommand(interp, "vfs_set_readmark", test_vfs_set_readmark, 0, 0);
  return TCL_OK;
}

#endif
