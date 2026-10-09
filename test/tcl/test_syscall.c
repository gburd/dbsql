/*
** 2011 March 28
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
** The code in this file implements a Tcl interface used to test error
** handling in the os_unix.c module. Wrapper functions that support fault
** injection are registered as the low-level OS functions using the 
** set_system_call() method of the VFS. The Tcl interface is as follows:
**
**
**   test_syscall install LIST
**     Install wrapper functions for all system calls in argument LIST.
**     LIST must be a list consisting of zero or more of the following
**     literal values:
**
**         open        close      access   getcwd   stat      fstat    
**         ftruncate   fcntl      read     pread    pread64   write
**         pwrite      pwrite64   fchmod   fallocate mmap
**
**   test_syscall uninstall
**     Uninstall all wrapper functions.
**
**   test_syscall fault ?COUNT PERSIST?
**     If [test_syscall fault] is invoked without the two arguments, fault
**     injection is disabled. Otherwise, fault injection is configured to
**     cause a failure on the COUNT'th next call to a system call with a
**     wrapper function installed. A COUNT value of 1 means fail the next
**     system call. 
** 
**     Argument PERSIST is interpreted as a boolean. If true, the all
**     system calls following the initial failure also fail. Otherwise, only
**     the single transient failure is injected.
**
**   test_syscall errno CALL ERRNO
**     Set the value that the global "errno" is set to following a fault
**     in call CALL. Argument CALL must be one of the system call names
**     listed above (under [test_syscall install]). ERRNO is a symbolic
**     name (i.e. "EACCES"). Not all errno codes are supported. Add extra
**     to the errnos table in function test_syscall_errno() below as 
**     required.
**
**   test_syscall reset ?SYSTEM-CALL?
**     with_t no argument, this is an alias for the [uninstall] command. However,
**     this command uses a VFS call of the form:
**
**       set_system_call(vfs, 0, 0);
**
**     To restore the default system calls. The [uninstall] command restores
**     each system call individually by calling (i.e.):
**
**       set_system_call(vfs, "open", 0);
**
**     with_t an argument, this command attempts to reset the system call named
**     by the parameter using the same method as [uninstall].
**
**   test_syscall exists SYSTEM-CALL
**     Return true if the named system call exists. Or false otherwise.
**
**   test_syscall list
**     Return a list of all system calls. The list is constructed using
**     the next_system_call() VFS method.
**
**   test_syscall pagesize PGSZ
**     If PGSZ is a power of two greater than 256, install a wrapper around
**     OS function getpagesize() that reports the system page size as PGSZ.
**     Or, if PGSZ is less than zero, remove any wrapper already installed.
*/

#include "dbsql_int.h"
#include "dbsql.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if DBSQL_OS_UNIX

/* From main.c */
extern const char *__err_name(int);

#include <sys/mman.h>
#include <sys/types.h>
#include <errno.h>

static struct test_syscall_global_t {
  int persist;                   /* 1 for persistent errors, 0 for transient */
  int n_count;                     /* Fail after this many more calls */
  int n_fail;                      /* Number of failures that have occurred */
  int pgsz;
  dbsql_syscall_ptr orig_getpagesize;
} __g_syscall = { 0, 0, 0, 0, 0 };

static int ts_open(const char *, int, int);
static int ts_close(int fd);
static int ts_access(const char *path, int mode);
static char *ts_getcwd(char *path, size_t n_path);
static int ts_stat(const char *path, struct stat *p);
static int ts_fstat(int fd, struct stat *p);
static int ts_ftruncate(int fd, off_t n);
static int ts_fcntl(int fd, int cmd, ... );
static ssize_t ts_read(int fd, void *bufs, size_t n_buf);
static ssize_t ts_pread(int fd, void *bufs, size_t n_buf, off_t off);
/* Note:  pread64() and pwrite64() actually use off64_t as the type on their
** last parameter.  But that datatype is not defined on many systems 
** (ex: Mac, __open_bsd).  So substitute a likely equivalent: u_int64_t */
static ssize_t ts_pread64(int fd, void *bufs, size_t n_buf, u_int64_t off);
static ssize_t ts_write(int fd, const void *bufs, size_t n_buf);
static ssize_t ts_pwrite(int fd, const void *bufs, size_t n_buf, off_t off);
static ssize_t ts_pwrite64(int fd, const void *bufs, size_t n_buf, u_int64_t off);
static int ts_fchmod(int fd, mode_t mode);
static int ts_fallocate(int fd, off_t off, off_t len);
static void *ts_mmap(void *, size_t, int, int, int, off_t);
static void *ts_mremap(void*, size_t, size_t, int, ...);

struct test_syscall_array_t {
  const char *name;
  dbsql_syscall_ptr test;
  dbsql_syscall_ptr orig;
  int default_errno;              /* Default value for errno following errors */
  int custom_errno;               /* Current value for errno if error */
} __a_syscall[] = {
  /*  0 */ { "open",      (dbsql_syscall_ptr)ts_open,      0, EACCES, 0 },
  /*  1 */ { "close",     (dbsql_syscall_ptr)ts_close,     0, 0, 0 },
  /*  2 */ { "access",    (dbsql_syscall_ptr)ts_access,    0, 0, 0 },
  /*  3 */ { "getcwd",    (dbsql_syscall_ptr)ts_getcwd,    0, 0, 0 },
  /*  4 */ { "stat",      (dbsql_syscall_ptr)ts_stat,      0, 0, 0 },
  /*  5 */ { "fstat",     (dbsql_syscall_ptr)ts_fstat,     0, 0, 0 },
  /*  6 */ { "ftruncate", (dbsql_syscall_ptr)ts_ftruncate, 0, EIO, 0 },
  /*  7 */ { "fcntl",     (dbsql_syscall_ptr)ts_fcntl,     0, EACCES, 0 },
  /*  8 */ { "read",      (dbsql_syscall_ptr)ts_read,      0, 0, 0 },
  /*  9 */ { "pread",     (dbsql_syscall_ptr)ts_pread,     0, 0, 0 },
  /* 10 */ { "pread64",   (dbsql_syscall_ptr)ts_pread64,   0, 0, 0 },
  /* 11 */ { "write",     (dbsql_syscall_ptr)ts_write,     0, 0, 0 },
  /* 12 */ { "pwrite",    (dbsql_syscall_ptr)ts_pwrite,    0, 0, 0 },
  /* 13 */ { "pwrite64",  (dbsql_syscall_ptr)ts_pwrite64,  0, 0, 0 },
  /* 14 */ { "fchmod",    (dbsql_syscall_ptr)ts_fchmod,    0, 0, 0 },
  /* 15 */ { "fallocate", (dbsql_syscall_ptr)ts_fallocate, 0, 0, 0 },
  /* 16 */ { "mmap",      (dbsql_syscall_ptr)ts_mmap,      0, 0, 0 },
  /* 17 */ { "mremap",    (dbsql_syscall_ptr)ts_mremap,    0, 0, 0 },
           { 0, 0, 0, 0, 0 }
};

#define orig_open      ((int(*)(const char *, int, int))__a_syscall[0].orig)
#define orig_close     ((int(*)(int))__a_syscall[1].orig)
#define orig_access    ((int(*)(const char*,int))__a_syscall[2].orig)
#define orig_getcwd    ((char*(*)(char*,size_t))__a_syscall[3].orig)
#define orig_stat      ((int(*)(const char*,struct stat*))__a_syscall[4].orig)
#define orig_fstat     ((int(*)(int,struct stat*))__a_syscall[5].orig)
#define orig_ftruncate ((int(*)(int,off_t))__a_syscall[6].orig)
#define orig_fcntl     ((int(*)(int,int,...))__a_syscall[7].orig)
#define orig_read      ((ssize_t(*)(int,void*,size_t))__a_syscall[8].orig)
#define orig_pread     ((ssize_t(*)(int,void*,size_t,off_t))__a_syscall[9].orig)
#define orig_pread64   ((ssize_t(*)(int,void*,size_t,u_int64_t))__a_syscall[10].orig)
#define orig_write     ((ssize_t(*)(int,const void*,size_t))__a_syscall[11].orig)
#define orig_pwrite    ((ssize_t(*)(int,const void*,size_t,off_t))\
                       __a_syscall[12].orig)
#define orig_pwrite64  ((ssize_t(*)(int,const void*,size_t,u_int64_t))\
                       __a_syscall[13].orig)
#define orig_fchmod    ((int(*)(int,mode_t))__a_syscall[14].orig)
#define orig_fallocate ((int(*)(int,off_t,off_t))__a_syscall[15].orig)
#define orig_mmap      ((void*(*)(void*,size_t,int,int,int,off_t))__a_syscall[16].orig)
#define orig_mremap    ((void*(*)(void*,size_t,size_t,int,...))__a_syscall[17].orig)

/*
** This function is called exactly once from within each invocation of a
** system call wrapper in this file. It returns 1 if the function should
** fail, or 0 if it should succeed.
*/
static int __ts_is_fail(void){
  __g_syscall.n_count--;
  if( __g_syscall.n_count==0 || (__g_syscall.n_fail && __g_syscall.persist) ){
    __g_syscall.n_fail++;
    return 1;
  }
  return 0;
}

/*
** Return the current error-number value for function func. func must be
** the name of a system call in the __a_syscall[] table.
**
** Usually, the current error-number is the value that errno should be set
** to if the named system call fails. The exception is "fallocate". See 
** comments above the implementation of ts_fallocate() for details.
*/
static int __ts_errno(const char *func){
  int i;
  size_t n_func = strlen(func);
  for(i=0; __a_syscall[i].name; i++){
    if( strlen(__a_syscall[i].name)!=n_func ) continue;
    if( memcmp(__a_syscall[i].name, func, n_func) ) continue;
    return __a_syscall[i].custom_errno;
  }

  assert(0);
  return 0;
}

/*
** A wrapper around __ts_is_fail(). If __ts_is_fail() returns non-zero, set the
** value of errno before returning.
*/
static int __ts_is_fail_errno(const char *func){
  if( __ts_is_fail() ){
    errno = __ts_errno(func);
    return 1;
  }
  return 0;
}

/*
** A wrapper around open().
*/
static int ts_open(const char *file, int flags, int mode){
  if( __ts_is_fail_errno("open") ){
    return -1;
  }
  return orig_open(file, flags, mode);
}

/*
** A wrapper around close().
*/
static int ts_close(int fd){
  if( __ts_is_fail() ){
    /* Even if simulating an error, close the original file-descriptor. 
    ** This is to stop the test process from running out of file-descriptors
    ** when running a long test. If a call to close() appears to fail, dbsql
    ** never attempts to use the file-descriptor afterwards (or even to close
    ** it a second time).  */
    orig_close(fd);
    return -1;
  }
  return orig_close(fd);
}

/*
** A wrapper around access().
*/
static int ts_access(const char *path, int mode){
  if( __ts_is_fail() ){
    return -1;
  }
  return orig_access(path, mode);
}

/*
** A wrapper around getcwd().
*/
static char *ts_getcwd(char *path, size_t n_path){
  if( __ts_is_fail() ){
    return NULL;
  }
  return orig_getcwd(path, n_path);
}

/*
** A wrapper around stat().
*/
static int ts_stat(const char *path, struct stat *p){
  if( __ts_is_fail() ){
    return -1;
  }
  return orig_stat(path, p);
}

/*
** A wrapper around fstat().
*/
static int ts_fstat(int fd, struct stat *p){
  if( __ts_is_fail_errno("fstat") ){
    return -1;
  }
  return orig_fstat(fd, p);
}

/*
** A wrapper around ftruncate().
*/
static int ts_ftruncate(int fd, off_t n){
  if( __ts_is_fail_errno("ftruncate") ){
    return -1;
  }
  return orig_ftruncate(fd, n);
}

/*
** A wrapper around fcntl().
*/
static int ts_fcntl(int fd, int cmd, ... ){
  va_list ap;
  void *arg;
  if( __ts_is_fail_errno("fcntl") ){
    return -1;
  }
  va_start(ap, cmd);
  arg = va_arg(ap, void *);
  return orig_fcntl(fd, cmd, arg);
}

/*
** A wrapper around read().
*/
static ssize_t ts_read(int fd, void *bufs, size_t n_buf){
  if( __ts_is_fail_errno("read") ){
    return -1;
  }
  return orig_read(fd, bufs, n_buf);
}

/*
** A wrapper around pread().
*/
static ssize_t ts_pread(int fd, void *bufs, size_t n_buf, off_t off){
  if( __ts_is_fail_errno("pread") ){
    return -1;
  }
  return orig_pread(fd, bufs, n_buf, off);
}

/*
** A wrapper around pread64().
*/
static ssize_t ts_pread64(int fd, void *bufs, size_t n_buf, u_int64_t off){
  if( __ts_is_fail_errno("pread64") ){
    return -1;
  }
  return orig_pread64(fd, bufs, n_buf, off);
}

/*
** A wrapper around write().
*/
static ssize_t ts_write(int fd, const void *bufs, size_t n_buf){
  if( __ts_is_fail_errno("write") ){
    if( __ts_errno("write")==EINTR ) orig_write(fd, bufs, n_buf/2);
    return -1;
  }
  return orig_write(fd, bufs, n_buf);
}

/*
** A wrapper around pwrite().
*/
static ssize_t ts_pwrite(int fd, const void *bufs, size_t n_buf, off_t off){
  if( __ts_is_fail_errno("pwrite") ){
    return -1;
  }
  return orig_pwrite(fd, bufs, n_buf, off);
}

/*
** A wrapper around pwrite64().
*/
static ssize_t ts_pwrite64(int fd, const void *bufs, size_t n_buf, u_int64_t off){
  if( __ts_is_fail_errno("pwrite64") ){
    return -1;
  }
  return orig_pwrite64(fd, bufs, n_buf, off);
}

/*
** A wrapper around fchmod().
*/
static int ts_fchmod(int fd, mode_t mode){
  if( __ts_is_fail() ){
    return -1;
  }
  return orig_fchmod(fd, mode);
}

/*
** A wrapper around fallocate().
**
** dbsql assumes that the fallocate() function is compatible with
** posix_fallocate(). According to the Linux man page (2009-09-30):
**
**   posix_fallocate() returns  zero on success, or an error number on
**   failure. Note that errno is not set.
*/
static int ts_fallocate(int fd, off_t off, off_t len){
  if( __ts_is_fail() ){
    return __ts_errno("fallocate");
  }
  return orig_fallocate(fd, off, len);
}

static void *ts_mmap(
  void *addr, 
  size_t n_byte, 
  int prot, 
  int flags, 
  int fd, 
  off_t i_off
){
  if( __ts_is_fail_errno("mmap") ){
    return MAP_FAILED;
  }
  return orig_mmap(addr, n_byte, prot, flags, fd, i_off);
}

static void *ts_mremap(void *a, size_t b, size_t c, int d, ...){
  va_list ap;
  void *arg;
  if( __ts_is_fail_errno("mremap") ){
    return MAP_FAILED;
  }
  va_start(ap, d);
  arg = va_arg(ap, void *);
  return orig_mremap(a, b, c, d, arg);
}

static int DBSQL_TCLAPI test_syscall_install(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs; 
  TCL_SIZE n_elem;
  int i;
  Tcl_Obj **elems;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 2, objv, "SYSCALL-LIST");
    return TCL_ERROR;
  }
  if( Tcl_ListObjGetElements(interp, objv[2], &n_elem, &elems) ){
    return TCL_ERROR;
  }
  vfs = dbsql_vfs_find(0);

  for(i=0; i<(int)n_elem; i++){
    int i_call;
    int rc = Tcl_GetIndexFromObjStruct(interp, 
        elems[i], __a_syscall, sizeof(__a_syscall[0]), "system-call", 0, &i_call
    );
    if( rc ) return rc;
    if( __a_syscall[i_call].orig==0 ){
      __a_syscall[i_call].orig = vfs->get_system_call(vfs, __a_syscall[i_call].name);
      vfs->set_system_call(vfs, __a_syscall[i_call].name, __a_syscall[i_call].test);
    }
    __a_syscall[i_call].custom_errno = __a_syscall[i_call].default_errno;
  }

  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_uninstall(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs; 
  int i;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }

  vfs = dbsql_vfs_find(0);
  for(i=0; __a_syscall[i].name; i++){
    if( __a_syscall[i].orig ){
      vfs->set_system_call(vfs, __a_syscall[i].name, 0);
      __a_syscall[i].orig = 0;
    }
  }
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_reset(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs; 
  int i;
  int rc;

  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }

  vfs = dbsql_vfs_find(0);
  if( objc==2 ){
    rc = vfs->set_system_call(vfs, 0, 0);
    for(i=0; __a_syscall[i].name; i++) __a_syscall[i].orig = 0;
  }else{
    TCL_SIZE n_func;
    char *func = Tcl_GetStringFromObj(objv[2], &n_func);
    rc = vfs->set_system_call(vfs, Tcl_GetString(objv[2]), 0);
    for(i=0; rc==DBSQL_OK && __a_syscall[i].name; i++){
      if( strlen(__a_syscall[i].name)!=n_func ) continue;
      if( memcmp(__a_syscall[i].name, func, n_func) ) continue;
      __a_syscall[i].orig = 0;
    }
  }
  if( rc!=DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    return TCL_ERROR;
  }

  Tcl_ResetResult(interp);
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_exists(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs; 
  dbsql_syscall_ptr x;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }

  vfs = dbsql_vfs_find(0);
  x = vfs->get_system_call(vfs, Tcl_GetString(objv[2]));

  Tcl_SetObjResult(interp, Tcl_NewBooleanObj(x!=0));
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_fault(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_count = 0;
  int persist = 0;

  if( objc!=2 && objc!=4 ){
    Tcl_WrongNumArgs(interp, 2, objv, "?COUNT PERSIST?");
    return TCL_ERROR;
  }

  if( objc==4 ){
    if( Tcl_GetIntFromObj(interp, objv[2], &n_count)
     || Tcl_GetBooleanFromObj(interp, objv[3], &persist)
    ){
      return TCL_ERROR;
    }
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(__g_syscall.n_fail));
  __g_syscall.n_count = n_count;
  __g_syscall.persist = persist;
  __g_syscall.n_fail = 0;
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_errno(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int i_call;
  int i_errno;
  int rc;

  struct errno_info_t {
    const char *z;
    int i;
  } errnos[] = {
    { "EACCES",    EACCES },
    { "EINTR",     EINTR },
    { "EIO",       EIO },
    { "EOVERFLOW", EOVERFLOW },
    { "ENOMEM",    ENOMEM },
    { "EAGAIN",    EAGAIN },
    { "ETIMEDOUT", ETIMEDOUT },
    { "EBUSY",     EBUSY },
    { "EPERM",     EPERM },
    { "EDEADLK",   EDEADLK },
    { "ENOLCK",    ENOLCK },
    { 0, 0 }
  };

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 2, objv, "SYSCALL ERRNO");
    return TCL_ERROR;
  }

  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[2], __a_syscall, sizeof(__a_syscall[0]), "system-call", 0, &i_call
  );
  if( rc!=TCL_OK ) return rc;
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[3], errnos, sizeof(errnos[0]), "errno", 0, &i_errno
  );
  if( rc!=TCL_OK ) return rc;

  __a_syscall[i_call].custom_errno = errnos[i_errno].i;
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_list(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *sys;
  dbsql_vfs_t *vfs; 
  Tcl_Obj *list;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }

  vfs = dbsql_vfs_find(0);
  list = Tcl_NewObj();
  Tcl_IncrRefCount(list);
  for(sys = vfs->next_system_call(vfs, 0); 
      sys!=0;
      sys = vfs->next_system_call(vfs, sys)
  ){
    Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj(sys, -1));
  }

  Tcl_SetObjResult(interp, list);
  Tcl_DecrRefCount(list);
  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall_defaultvfs(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs; 

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }

  vfs = dbsql_vfs_find(0);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(vfs->name, -1));
  return TCL_OK;
}

static int ts_getpagesize(void){
  return __g_syscall.pgsz;
}

static int DBSQL_TCLAPI test_syscall_pagesize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_vfs_t *vfs = dbsql_vfs_find(0);
  int pgsz;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 2, objv, "PGSZ");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &pgsz) ){
    return TCL_ERROR;
  }

  if( pgsz<0 ){
    if( __g_syscall.orig_getpagesize ){
      vfs->set_system_call(vfs, "getpagesize", __g_syscall.orig_getpagesize);
    }
  }else{
    if( pgsz<512 || (pgsz & (pgsz-1)) ){
      Tcl_AppendResult(interp, "pgsz out of range", NULL);
      return TCL_ERROR;
    }
    __g_syscall.orig_getpagesize = vfs->get_system_call(vfs, "getpagesize");
    __g_syscall.pgsz = pgsz;
    vfs->set_system_call(
        vfs, "getpagesize", (dbsql_syscall_ptr)ts_getpagesize
    );
  }

  return TCL_OK;
}

static int DBSQL_TCLAPI test_syscall(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct syscall_cmd_t {
    const char *name;
    Tcl_ObjCmdProc *cmd;
  } cmds[] = {
    { "fault",      test_syscall_fault },
    { "install",    test_syscall_install },
    { "uninstall",  test_syscall_uninstall },
    { "reset",      test_syscall_reset },
    { "errno",      test_syscall_errno },
    { "exists",     test_syscall_exists },
    { "list",       test_syscall_list },
    { "defaultvfs", test_syscall_defaultvfs },
    { "pagesize",   test_syscall_pagesize },
    { 0, 0 }
  };
  int i_cmd;
  int rc;
  dbsql_vfs_t *vfs = dbsql_vfs_find(0);

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND ...");
    return TCL_ERROR;
  }
  if( vfs->i_version<3 || vfs->set_system_call==0 ){
    Tcl_AppendResult(interp, "VFS does not support xSetSystemCall", NULL);
    rc = TCL_ERROR;
  }else{
    rc = Tcl_GetIndexFromObjStruct(interp, 
        objv[1], cmds, sizeof(cmds[0]), "sub-command", 0, &i_cmd
    );
  }
  if( rc!=TCL_OK ) return rc;
  return cmds[i_cmd].cmd(clientData, interp, objc, objv);
}

int DbsqltestSyscall_Init(Tcl_Interp *interp){
  struct syscall_cmd_t {
    const char *name;
    Tcl_ObjCmdProc *cmd;
  } cmds[] = {
    { "test_syscall",     test_syscall},
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateObjCommand(interp, cmds[i].name, cmds[i].cmd, 0, 0);
  }
  return TCL_OK;
}
#else
int DbsqltestSyscall_Init(Tcl_Interp *interp){
  return TCL_OK;
}
#endif
