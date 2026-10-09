/*
** 2004 May 22
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
** This file contains the VFS implementation for unix-like operating systems
** include Linux, __mac_osx, *BSD, QNX, __vx_works, AIX, HPUX, and others.
**
** There are actually several different VFS implementations in this file.
** The differences are in the way that file locking is done.  The default
** implementation uses Posix Advisory Locks.  Alternative implementations
** use flock(), dot-files, various proprietary locking schemas, or simply
** skip locking all together.
**
** This source file is organized into divisions where the logic for various
** subfunctions is contained within the appropriate division.  PLEASE
** KEEP THE STRUCTURE OF THIS FILE INTACT.  New code should be placed
** in the correct division and should be clearly labelled.
**
** The layout of divisions is as follows:
**
**   *  General-purpose declarations and utility functions.
**   *  Unique file ID logic used by __vx_works.
**   *  Various locking primitive implementations (all except proxy locking):
**      + for Posix Advisory Locks
**      + for no-op locks
**      + for dot-file locks
**      + for flock() locking
**      + for named semaphore locks (__vx_works only)
**      + for AFP filesystem locks (__mac_osx only)
**   *  dbsql_file_t methods not associated with locking.
**   *  Definitions of dbsql_io_methods_t objects for all locking
**      methods plus "finder" functions for each locking method.
**   *  dbsql_vfs_t method implementations.
**   *  Locking primitives for the proxy uber-locking-method. (__mac_osx only)
**   *  Definitions of dbsql_vfs_t objects for all locking methods
**      plus implementations of dbsql_os_init() and dbsql_os_end().
*/
#include "dbsql_int.h"
#if DBSQL_OS_UNIX              /* This file is used on unix only */

/*
** There are various methods for file locking used for concurrency
** control:
**
**   1. POSIX locking (the default),
**   2. No locking,
**   3. Dot-file locking,
**   4. flock() locking,
**   5. AFP locking (OSX only),
**   6. Named POSIX semaphores (VXWorks only),
**   7. proxy locking. (OSX only)
**
** Styles 4, 5, and 7 are only available of DBSQL_ENABLE_LOCKING_STYLE
** is defined to 1.  The DBSQL_ENABLE_LOCKING_STYLE also enables automatic
** selection of the appropriate locking style based on the filesystem
** where the database is located.
*/
#if !defined(DBSQL_ENABLE_LOCKING_STYLE)
#  if defined(__APPLE__)
#    define DBSQL_ENABLE_LOCKING_STYLE 1
#  else
#    define DBSQL_ENABLE_LOCKING_STYLE 0
#  endif
#endif

/* Use pread() and pwrite() if they are available */
#if defined(__APPLE__) || defined(__linux__)
# define HAVE_PREAD 1
# define HAVE_PWRITE 1
#endif
#if defined(HAVE_PREAD64) && defined(HAVE_PWRITE64)
# undef USE_PREAD
# define USE_PREAD64 1
#elif defined(HAVE_PREAD) && defined(HAVE_PWRITE)
# undef USE_PREAD64
# define USE_PREAD 1
#endif

/*
** standard include files.
*/
#include <sys/types.h>   /* amalgamator: keep */
#include <sys/stat.h>    /* amalgamator: keep */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>      /* amalgamator: keep */
#include <time.h>
#include <sys/time.h>    /* amalgamator: keep */
#include <errno.h>
#if (!defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0) \
  && !defined(DBSQL_WASI)
# include <sys/mman.h>
#endif

#if DBSQL_ENABLE_LOCKING_STYLE
# include <sys/ioctl.h>
# include <sys/file.h>
# include <sys/param.h>
#endif /* DBSQL_ENABLE_LOCKING_STYLE */

/*
** Try to determine if gethostuuid() is available based on standard
** macros.  This might sometimes compute the wrong value for some
** obscure platforms.  For those cases, simply compile with one of
** the following:
**
**    -DHAVE_GETHOSTUUID=0
**    -DHAVE_GETHOSTUUID=1
**
** None if this matters except when building on Apple products with
** -DSQLITE_ENABLE_LOCKING_STYLE.
*/
#ifndef HAVE_GETHOSTUUID
# define HAVE_GETHOSTUUID 0
# if defined(__APPLE__) && ((__MAC_OS_X_VERSION_MIN_REQUIRED > 1050) || \
                            (__IPHONE_OS_VERSION_MIN_REQUIRED > 2000))
#    if (!defined(TARGET_OS_EMBEDDED) || (TARGET_OS_EMBEDDED==0)) \
        && (!defined(TARGET_IPHONE_SIMULATOR) || (TARGET_IPHONE_SIMULATOR==0))\
        && (!defined(TARGET_OS_MACCATALYST) || (TARGET_OS_MACCATALYST==0))
#      undef HAVE_GETHOSTUUID
#      define HAVE_GETHOSTUUID 1
#    else
#      warning "gethostuuid() is disabled."
#    endif
#  endif
#endif


#if OS_VXWORKS
# include <sys/ioctl.h>
# include <semaphore.h>
# include <limits.h>
#endif /* OS_VXWORKS */

#if defined(__APPLE__) || DBSQL_ENABLE_LOCKING_STYLE
# include <sys/mount.h>
#endif

#ifdef HAVE_UTIME
# include <utime.h>
#endif

/*
** Allowed values of unix_file.fs_flags
*/
#define DBSQL_FSFLAGS_IS_MSDOS     0x1

/*
** If we are to be thread-safe, include the pthreads header.
*/
#if DBSQL_THREADSAFE
# include <pthread.h>
#endif

/*
** Default permissions when creating a new file
*/
#ifndef DBSQL_DEFAULT_FILE_PERMISSIONS
# define DBSQL_DEFAULT_FILE_PERMISSIONS 0644
#endif

/*
** Default permissions when creating auto proxy dir
*/
#ifndef DBSQL_DEFAULT_PROXYDIR_PERMISSIONS
# define DBSQL_DEFAULT_PROXYDIR_PERMISSIONS 0755
#endif

/*
** Maximum supported path-length.
*/
#define MAX_PATHNAME 512

/*
** Maximum supported symbolic links
*/
#define DBSQL_MAX_SYMLINKS 100

/*
** Remove and stub certain info for WASI (__web_assembly System
** Interface) builds.
*/
#ifdef DBSQL_WASI
# undef HAVE_FCHMOD
# undef HAVE_FCHOWN
# undef HAVE_MREMAP
# define HAVE_MREMAP 0
# ifndef DBSQL_DEFAULT_UNIX_VFS
#  define DBSQL_DEFAULT_UNIX_VFS "unix-dotfile"
   /* ^^^ should DBSQL_DEFAULT_UNIX_VFS be "unix-none"? */
# endif
# ifndef F_RDLCK
#  define F_RDLCK 0
#  define F_WRLCK 1
#  define F_UNLCK 2
#  if __LONG_MAX == 0x7fffffffL
#   define F_GETLK 12
#   define F_SETLK 13
#   define F_SETLKW 14
#  else
#   define F_GETLK 5
#   define F_SETLK 6
#   define F_SETLKW 7
#  endif
# endif
#else /* !DBSQL_WASI */
# ifndef HAVE_FCHMOD
#  define HAVE_FCHMOD 1
# endif
#endif /* DBSQL_WASI */

#ifdef DBSQL_WASI
# define OS_GETPID(X) (pid_t)1
#else
/* Always cast the getpid() return type for compatibility with
** kernel modules in __vx_works. */
# define OS_GETPID(X) (pid_t)getpid()
#endif

/*
** Only set the last_errno if the error code is a real error and not
** a normal expected return code of DBSQL_BUSY or DBSQL_OK
*/
#define IS_LOCK_ERROR(x)  ((x != DBSQL_OK) && (x != DBSQL_BUSY))

/* Forward references */
typedef struct unix_shm unix_shm;               /* Connection shared memory */
typedef struct unix_shm_node unix_shm_node;       /* Shared memory instance */
typedef struct unix_inode_info unix_inode_info;   /* An i-node */
typedef struct unix_unused_fd_t unix_unused_fd_t;     /* An unused file descriptor */

/*
** Sometimes, after a file handle is closed by dbsql, the file descriptor
** cannot be closed immediately. In these cases, instances of the following
** structure are used to store the file descriptor while waiting for an
** opportunity to either close or reuse it.
*/
struct unix_unused_fd_t {
  int fd;                   /* File descriptor to close */
  int flags;                /* Flags this file descriptor was opened with */
  unix_unused_fd_t *next;      /* Next unused file descriptor on same file */
};

/*
** The unix_file structure is subclass of dbsql_file_t specific to the unix
** VFS implementations.
*/
typedef struct unix_file unix_file;
struct unix_file {
  dbsql_io_methods_t const *method;  /* Always the first entry */
  dbsql_vfs_t *vfs;                  /* The VFS that created this unix_file */
  unix_inode_info *inode;              /* Info about locks on this inode */
  int h;                              /* The file descriptor */
  unsigned char file_lock;            /* The type of lock held on this fd */
  unsigned short int ctrl_flags;       /* Behavioral bits.  UNIXFILE_* flags */
  int last_errno;                      /* The unix errno from last I/O error */
  void *locking_context;               /* Locking style specific state */
  unix_unused_fd_t *preallocated_unused;  /* Pre-allocated unix_unused_fd_t */
  const char *path;                  /* Name of the file */
  unix_shm *shm;                      /* Shared memory segment information */
  int sz_chunk;                        /* Configured by FCNTL_CHUNK_SIZE */
#if DBSQL_MAX_MMAP_SIZE>0
  int n_fetch_out;                      /* Number of outstanding fetch refs */
  int64_t mmap_size;             /* Usable size of mapping at map_region */
  int64_t mmap_size_actual;       /* Actual size of mapping at map_region */
  int64_t mmap_size_max;          /* Configured FCNTL_MMAP_SIZE value */
  void *map_region;                   /* Memory mapped region */
#endif
  int sector_size;                     /* Device sector size */
  int device_characteristics;          /* Precomputed device characteristics */
#if DBSQL_ENABLE_LOCKING_STYLE
  int open_flags;                      /* The flags specified at open() */
#endif
#if DBSQL_ENABLE_LOCKING_STYLE || defined(__APPLE__)
  unsigned fs_flags;                   /* cached details from statfs() */
#endif
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  unsigned i_busy_timeout;              /* Wait this many millisec on locks */
  int block_on_connect;                /* True to block for SHARED locks */
#endif
#if OS_VXWORKS
  struct vxworks_file_id *id;          /* Unique file ID */
#endif
#ifdef DBSQL_DEBUG
  /* The next group of variables are used to track whether or not the
  ** transaction counter in bytes 24-27 of database files are updated
  ** whenever any part of the database changes.  An assertion fault will
  ** occur if a file is updated without also updating the transaction
  ** counter.  This test is made to avoid new problems similar to the
  ** one described by ticket #3584.
  */
  unsigned char trans_cntr_chng;   /* True if the transaction counter changed */
  unsigned char db_update;        /* True if any part of database file changed */
  unsigned char in_normal_write;   /* True if in a normal write operation */

#endif

#ifdef DBSQL_TEST
  /* In test mode, increase the size of this structure a bit so that
  ** it is larger than the struct crash_file_t defined in test6.c.
  */
  char paddings[32];
#endif
};

/* This variable holds the process id (pid) from when the randomness()
** method was called.  If x_open() is called from a different process id,
** indicating that a fork() has occurred, the PRNG will be reset.
*/
static pid_t __randomness_pid = 0;

/*
** Allowed values for the unix_file.ctrl_flags bitmask:
*/
#define UNIXFILE_EXCL        0x01     /* Connections from one process only */
#define UNIXFILE_RDONLY      0x02     /* Connection is read only */
#define UNIXFILE_PERSIST_WAL 0x04     /* Persistent WAL mode */
#if !defined(DBSQL_DISABLE_DIRSYNC) && !defined(_AIX)
# define UNIXFILE_DIRSYNC    0x08     /* Directory sync needed */
#else
# define UNIXFILE_DIRSYNC    0x00
#endif
#define UNIXFILE_PSOW        0x10     /* DBSQL_IOCAP_POWERSAFE_OVERWRITE */
#define UNIXFILE_DELETE      0x20     /* Delete on close */
#define UNIXFILE_URI         0x40     /* Filename might have query parameters */
#define UNIXFILE_NOLOCK      0x80     /* Do no file locking */

/*
** Include code that is common to all os_*.c files
*/
#include "os_common.h"

/*
** Define various macros that are missing from some systems.
*/
#ifndef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifdef DBSQL_DISABLE_LFS
# undef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
#ifndef O_BINARY
# define O_BINARY 0
#endif

/*
** The threadid macro resolves to the thread-id or to 0.  Used for
** testing and debugging only.
*/
#if DBSQL_THREADSAFE
#define threadid pthread_self()
#else
#define threadid 0
#endif

/*
** HAVE_MREMAP defaults to true on Linux and false everywhere else.
*/
#if !defined(HAVE_MREMAP)
# if defined(__linux__) && defined(_GNU_SOURCE)
#  define HAVE_MREMAP 1
# else
#  define HAVE_MREMAP 0
# endif
#endif

/*
** Explicitly call the 64-bit version of lseek() on Android. Otherwise, lseek()
** is the 32-bit version, even if _FILE_OFFSET_BITS=64 is defined.
*/
#ifdef __ANDROID__
# define lseek lseek64
#endif

#ifdef __linux__
/*
** Linux-specific IOCTL magic numbers used for controlling F2FS
*/
#define F2FS_IOCTL_MAGIC        0xf5
#define F2FS_IOC_START_ATOMIC_WRITE     _IO(F2FS_IOCTL_MAGIC, 1)
#define F2FS_IOC_COMMIT_ATOMIC_WRITE    _IO(F2FS_IOCTL_MAGIC, 2)
#define F2FS_IOC_START_VOLATILE_WRITE   _IO(F2FS_IOCTL_MAGIC, 3)
#define F2FS_IOC_ABORT_VOLATILE_WRITE   _IO(F2FS_IOCTL_MAGIC, 5)
#define F2FS_IOC_GET_FEATURES           _IOR(F2FS_IOCTL_MAGIC, 12, u_int32_t)
#define F2FS_FEATURE_ATOMIC_WRITE 0x0004
#endif /* __linux__ */


/*
** Different Unix systems declare open() in different ways.  Same use
** open(const char*,int,mode_t).  Others use open(const char*,int,...).
** The difference is important when using a pointer to the function.
**
** The safest way to deal with the problem is to always use this wrapper
** which always has the same well-defined interface.
*/
static int __posix_open(const char *file, int flags, int mode){
  return open(file, flags, mode);
}

/* Forward reference */
static int __open_directory(const char*, int*);
static int __unix_getpagesize(void);

/*
** Many system calls are accessed through pointer-to-functions so that
** they may be overridden at runtime to facilitate fault injection during
** testing and sandboxing.  The following array holds the names and pointers
** to all overrideable system calls.
*/
static struct unix_syscall {
  const char *name;            /* Name of the system call */
  dbsql_syscall_ptr current; /* Current value of the system call */
  dbsql_syscall_ptr p_default; /* Default value */
} __a_syscall[] = {
  { "open",         (dbsql_syscall_ptr)__posix_open,  0  },
#define OS_OPEN      ((int(*)(const char*,int,int))__a_syscall[0].current)

  { "close",        (dbsql_syscall_ptr)close,      0  },
#define OS_CLOSE     ((int(*)(int))__a_syscall[1].current)

  { "access",       (dbsql_syscall_ptr)access,     0  },
#define OS_ACCESS    ((int(*)(const char*,int))__a_syscall[2].current)

  { "getcwd",       (dbsql_syscall_ptr)getcwd,     0  },
#define OS_GETCWD    ((char*(*)(char*,size_t))__a_syscall[3].current)

  { "stat",         (dbsql_syscall_ptr)stat,       0  },
#define OS_STAT      ((int(*)(const char*,struct stat*))__a_syscall[4].current)

/*
** The DJGPP compiler environment looks mostly like Unix, but it
** lacks the fcntl() system call.  So redefine fcntl() to be something
** that always succeeds.  This means that locking does not occur under
** DJGPP.  But it is DOS - what did you expect?
*/
#ifdef __DJGPP__
  { "fstat",        0,                 0  },
#define OS_FSTAT(a,b,c)    0
#else   
  { "fstat",        (dbsql_syscall_ptr)fstat,      0  },
#define OS_FSTAT     ((int(*)(int,struct stat*))__a_syscall[5].current)
#endif

  { "ftruncate",    (dbsql_syscall_ptr)ftruncate,  0  },
#define OS_FTRUNCATE ((int(*)(int,off_t))__a_syscall[6].current)

  { "fcntl",        (dbsql_syscall_ptr)fcntl,      0  },
#define __os_fcntl     ((int(*)(int,int,...))__a_syscall[7].current)

  { "read",         (dbsql_syscall_ptr)read,       0  },
#define OS_READ      ((ssize_t(*)(int,void*,size_t))__a_syscall[8].current)

#if defined(USE_PREAD) || DBSQL_ENABLE_LOCKING_STYLE
  { "pread",        (dbsql_syscall_ptr)pread,      0  },
#else
  { "pread",        (dbsql_syscall_ptr)0,          0  },
#endif
#define OS_PREAD     ((ssize_t(*)(int,void*,size_t,off_t))__a_syscall[9].current)

#if defined(USE_PREAD64)
  { "pread64",      (dbsql_syscall_ptr)pread64,    0  },
#else
  { "pread64",      (dbsql_syscall_ptr)0,          0  },
#endif
#define OS_PREAD64 ((ssize_t(*)(int,void*,size_t,off64_t))__a_syscall[10].current)

  { "write",        (dbsql_syscall_ptr)write,      0  },
#define OS_WRITE     ((ssize_t(*)(int,const void*,size_t))__a_syscall[11].current)

#if defined(USE_PREAD) || DBSQL_ENABLE_LOCKING_STYLE
  { "pwrite",       (dbsql_syscall_ptr)pwrite,     0  },
#else
  { "pwrite",       (dbsql_syscall_ptr)0,          0  },
#endif
#define OS_PWRITE    ((ssize_t(*)(int,const void*,size_t,off_t))\
                    __a_syscall[12].current)

#if defined(USE_PREAD64)
  { "pwrite64",     (dbsql_syscall_ptr)pwrite64,   0  },
#else
  { "pwrite64",     (dbsql_syscall_ptr)0,          0  },
#endif
#define OS_PWRITE64  ((ssize_t(*)(int,const void*,size_t,off64_t))\
                    __a_syscall[13].current)

#if defined(HAVE_FCHMOD)
  { "fchmod",       (dbsql_syscall_ptr)fchmod,          0  },
#define OS_FCHMOD    ((int(*)(int,mode_t))__a_syscall[14].current)
#else
  { "fchmod",       (dbsql_syscall_ptr)0,               0  },
#define OS_FCHMOD(FID,MODE) 0
#endif

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
  { "fallocate",    (dbsql_syscall_ptr)posix_fallocate,  0 },
#else
  { "fallocate",    (dbsql_syscall_ptr)0,                0 },
#endif
#define OS_FALLOCATE ((int(*)(int,off_t,off_t))__a_syscall[15].current)

  { "unlink",       (dbsql_syscall_ptr)unlink,           0 },
#define OS_UNLINK    ((int(*)(const char*))__a_syscall[16].current)

  { "openDirectory",    (dbsql_syscall_ptr)__open_directory,      0 },
#define OS_OPEN_DIRECTORY ((int(*)(const char*,int*))__a_syscall[17].current)

  { "mkdir",        (dbsql_syscall_ptr)mkdir,           0 },
#define OS_MKDIR     ((int(*)(const char*,mode_t))__a_syscall[18].current)

  { "rmdir",        (dbsql_syscall_ptr)rmdir,           0 },
#define OS_RMDIR     ((int(*)(const char*))__a_syscall[19].current)

#if defined(HAVE_FCHOWN)
  { "fchown",       (dbsql_syscall_ptr)fchown,          0 },
#else
  { "fchown",       (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_FCHOWN    ((int(*)(int,uid_t,gid_t))__a_syscall[20].current)

#if defined(HAVE_FCHOWN)
  { "geteuid",      (dbsql_syscall_ptr)geteuid,         0 },
#else
  { "geteuid",      (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_GETEUID   ((uid_t(*)(void))__a_syscall[21].current)

#if (!defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0) \
  && !defined(DBSQL_WASI)
  { "mmap",         (dbsql_syscall_ptr)mmap,            0 },
#else
  { "mmap",         (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_MMAP ((void*(*)(void*,size_t,int,int,int,off_t))__a_syscall[22].current)

#if (!defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0) \
  && !defined(DBSQL_WASI)
  { "munmap",       (dbsql_syscall_ptr)munmap,          0 },
#else
  { "munmap",       (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_MUNMAP ((int(*)(void*,size_t))__a_syscall[23].current)

#if HAVE_MREMAP && (!defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0)
  { "mremap",       (dbsql_syscall_ptr)mremap,          0 },
#else
  { "mremap",       (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_MREMAP ((void*(*)(void*,size_t,size_t,int,...))__a_syscall[24].current)

#if !defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0
  { "getpagesize",  (dbsql_syscall_ptr)__unix_getpagesize, 0 },
#else
  { "getpagesize",  (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_GETPAGESIZE ((int(*)(void))__a_syscall[25].current)

#if defined(HAVE_READLINK)
  { "readlink",     (dbsql_syscall_ptr)readlink,        0 },
#else
  { "readlink",     (dbsql_syscall_ptr)0,               0 },
#endif
#define OS_READLINK ((ssize_t(*)(const char*,char*,size_t))__a_syscall[26].current)

#if defined(HAVE_LSTAT)
  { "lstat",         (dbsql_syscall_ptr)lstat,          0 },
#else
  { "lstat",         (dbsql_syscall_ptr)0,              0 },
#endif
#define OS_LSTAT      ((int(*)(const char*,struct stat*))__a_syscall[27].current)

#if defined(__linux__) && defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
# ifdef __ANDROID__
  { "ioctl", (dbsql_syscall_ptr)(int(*)(int, int, ...))ioctl, 0 },
#define OS_IOCTL ((int(*)(int,int,...))__a_syscall[28].current)
# else
  { "ioctl",         (dbsql_syscall_ptr)ioctl,          0 },
#define OS_IOCTL ((int(*)(int,unsigned long,...))__a_syscall[28].current)
# endif
#else
  { "ioctl",         (dbsql_syscall_ptr)0,              0 },
#endif

}; /* End of the overrideable system calls */


#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
/*
** Extract Posix Advisory Locking information about file description fd
** from the /proc/PID/fdinfo/FD pseudo-file.  Fill the string buffer a[16]
** with characters to indicate which dbsql-relevant locks are held.
** a[16] will be a 15-character zero-terminated string with the following
** schema:
**
**     AAA/B.DDD.DDDDD
**
** Each of character A-D will be "w" or "r" or "-" to indicate either a
** write-lock, a read-lock, or no-lock, respectively.  The "." and "/"
** characters are delimiters intended to make the string more easily
** readable by humans.  Here are the meaning of the specific letters:
**
**     AAA   ->    The main database locks.  PENDING_BYTE, RESERVED_BYTE,
**                 and SHARED_FIRST, respectively.
**
**     B     ->    The deadman switch lock.  Offset 128 of the -shm file.
**
**     CCC   ->    WAL locks:  WRITE, CKPT, RECOVER
**
**     DDDDD ->    WAL read-locks 0 through 5
**
** Note that elements before the "/" apply to the main database file and
** elements after the "/" apply to the -shm file in WAL mode.
**
** Here is another way of thinking about the meaning of the result string:
**
**           AAA/B.CCC.DDDDD
**           ||| | ||| \___/
**  PENDING--'|| | |||   `----- READ 0-5
**  RESERVED--'| | ||`---- RECOVER
**  SHARED ----' | |`----- CKPT
**     DMS ------' `------ WRITE
**
** Return DBSQL_OK on success and DBSQL_ERROR_UNABLE if the /proc
** pseudo-filesystem is unavailable.
*/
static int UNIX_POSIX_ADVISORY_LOCKS(
  int fd,        /* The file descriptor to analyze */
  char a[16]     /* Write a text description of PALs here */
){
  int in;
  ssize_t n;
  char *p, *next, *x;
  char z[2000];

        /*             1     */
        /*   012 4 678 01234 */
  memcpy(a, "---/-.---.-----", 16);
  dbsql_snprintf(sizeof(z), z, "/proc/%d/fdinfo/%d", getpid(), fd);
  in = OS_OPEN(z, O_RDONLY, 0);
  if( in<0 ){
    return DBSQL_ERROR_UNABLE;
  }
  n = OS_READ(in, z, sizeof(z)-1);
  OS_CLOSE(in);
  if( n<=0 ) return DBSQL_ERROR_UNABLE;
  z[n] = 0;

  /* We are looking for lines that begin with "lock:\t".  Examples:
  **
  ** lock: 1: POSIX  ADVISORY  READ 494716 08:02:5277597 1073741826 1073742335
  ** lock: 1: POSIX  ADVISORY  WRITE 494716 08:02:5282282 120 120
  ** lock: 2: POSIX  ADVISORY  READ 494716 08:02:5282282 123 123
  ** lock: 3: POSIX  ADVISORY  READ 494716 08:02:5282282 128 128
  */
  next = strstr(z, "lock:\t");
  while( next ){
    char c_type = 0;
    int64_t i_first, i_last;
    p = next+6;
    next = strstr(p, "lock:\t");
    if( next ) next[-1] = 0;
    if( (x = strstr(p, " READ "))!=0 ){
      c_type = 'r';
      x += 6;
    }else if( (x = strstr(p, " WRITE "))!=0 ){
      c_type = 'w';
      x += 7;
    }else{
      continue;
    }
    x = strrchr(x, ' ');
    if( x==0 ) continue;
    i_last = strtoll(x+1, 0, 10);
    *x = 0;
    x = strrchr(p, ' ');
    if( x==0 ) continue;
    i_first = strtoll(x+1, 0, 10);
    if( i_last>=PENDING_BYTE ){
      if( i_first<=PENDING_BYTE && i_last>=PENDING_BYTE )     a[0] = c_type;
      if( i_first<=PENDING_BYTE+1 && i_last>=PENDING_BYTE+1 ) a[1] = c_type;
      if( i_first<=PENDING_BYTE+2 && i_last>=PENDING_BYTE+510 ) a[2] = c_type;
    }else if( i_last<=128 ){
      if( i_first<=128 && i_last>=128 ) a[4] = c_type;
      if( i_first<=120 && i_last>=120 ) a[6] = c_type;
      if( i_first<=121 && i_last>=121 ) a[7] = c_type;
      if( i_first<=122 && i_last>=122 ) a[8] = c_type;
      if( i_first<=123 && i_last>=123 ) a[10] = c_type;
      if( i_first<=124 && i_last>=124 ) a[11] = c_type;
      if( i_first<=125 && i_last>=125 ) a[12] = c_type;
      if( i_first<=126 && i_last>=126 ) a[13] = c_type;
      if( i_first<=127 && i_last>=127 ) a[14] = c_type;
    }
  }
  return DBSQL_OK;
}
#else
#  define UNIX_POSIX_ADVISORY_LOCKS(A,B) DBSQL_ERROR_UNABLE
#endif /* DBSQL_DEBUG || DBSQL_ENABLE_FILESTAT */

/*
** On some systems, calls to fchown() will trigger a message in a security
** log if they come from non-root processes.  So avoid calling fchown() if
** we are not running as root.
*/
static int __robust_fchown(int fd, uid_t uid, gid_t gid){
#if defined(HAVE_FCHOWN)
  return OS_GETEUID() ? 0 : OS_FCHOWN(fd,uid,gid);
#else
  return 0;
#endif
}

/*
** This is the set_system_call() method of dbsql_vfs_t for all of the
** "unix" VFSes.  Return DBSQL_OK upon successfully updating the
** system call pointer, or DBSQL_NOTFOUND if there is no configurable
** system call named name.
*/
static int __unix_set_system_call(
  dbsql_vfs_t *not_used,        /* The VFS pointer.  Not used */
  const char *name,            /* Name of system call to override */
  dbsql_syscall_ptr new_func  /* Pointer to new system call value */
){
  unsigned int i;
  int rc = DBSQL_NOTFOUND;

  UNUSED_PARAMETER(not_used);
  if( name==0 ){
    /* If no name is given, restore all system calls to their default
    ** settings and return NULL
    */
    rc = DBSQL_OK;
    for(i=0; i<sizeof(__a_syscall)/sizeof(__a_syscall[0]); i++){
      if( __a_syscall[i].p_default ){
        __a_syscall[i].current = __a_syscall[i].p_default;
      }
    }
  }else{
    /* If name is specified, operate on only the one system call
    ** specified.
    */
    for(i=0; i<sizeof(__a_syscall)/sizeof(__a_syscall[0]); i++){
      if( strcmp(name, __a_syscall[i].name)==0 ){
        if( __a_syscall[i].p_default==0 ){
          __a_syscall[i].p_default = __a_syscall[i].current;
        }
        rc = DBSQL_OK;
        if( new_func==0 ) new_func = __a_syscall[i].p_default;
        __a_syscall[i].current = new_func;
        break;
      }
    }
  }
  return rc;
}

/*
** Return the value of a system call.  Return NULL if name is not a
** recognized system call name.  NULL is also returned if the system call
** is currently undefined.
*/
static dbsql_syscall_ptr __unix_get_system_call(
  dbsql_vfs_t *not_used,
  const char *name
){
  unsigned int i;

  UNUSED_PARAMETER(not_used);
  for(i=0; i<sizeof(__a_syscall)/sizeof(__a_syscall[0]); i++){
    if( strcmp(name, __a_syscall[i].name)==0 ) return __a_syscall[i].current;
  }
  return 0;
}

/*
** Return the name of the first system call after name.  If name==NULL
** then return the name of the first system call.  Return NULL if name
** is the last system call or if name is not the name of a valid
** system call.
*/
static const char *__unix_next_system_call(dbsql_vfs_t *p, const char *name){
  int i = -1;

  UNUSED_PARAMETER(p);
  if( name ){
    for(i=0; i<ARRAY_SIZE(__a_syscall)-1; i++){
      if( strcmp(name, __a_syscall[i].name)==0 ) break;
    }
  }
  for(i++; i<ARRAY_SIZE(__a_syscall); i++){
    if( __a_syscall[i].current!=0 ) return __a_syscall[i].name;
  }
  return 0;
}

/*
** Do not accept any file descriptor less than this value, in order to avoid
** opening database file using file descriptors that are commonly used for
** standard input, output, and error.
*/
#ifndef DBSQL_MINIMUM_FILE_DESCRIPTOR
# define DBSQL_MINIMUM_FILE_DESCRIPTOR 3
#endif

/*
** Invoke open().  Do so multiple times, until it either succeeds or
** fails for some reason other than EINTR.
**
** If the file creation mode "m" is 0 then set it to the default for
** dbsql.  The default is DBSQL_DEFAULT_FILE_PERMISSIONS (normally
** 0644) as modified by the system umask.  If m is not 0, then
** make the file creation mode be exactly m ignoring the umask.
**
** The m parameter will be non-zero only when creating -wal, -journal,
** and -shm files.  We want those files to have *exactly* the same
** permissions as their original database, unadulterated by the umask.
** In that way, if a database file is -rw-rw-rw or -rw-rw-r-, and a
** transaction crashes and leaves behind hot journals, then any
** process that is able to write to the database will also be able to
** recover the hot journals.
*/
static int robust_open(const char *z, int f, mode_t m){
  int fd;
  mode_t m2 = m ? m : DBSQL_DEFAULT_FILE_PERMISSIONS;
  while(1){
#if defined(O_CLOEXEC)
    fd = OS_OPEN(z,f|O_CLOEXEC,m2);
#else
    fd = OS_OPEN(z,f,m2);
#endif
    if( fd<0 ){
      if( errno==EINTR ) continue;
      break;
    }
    if( fd>=DBSQL_MINIMUM_FILE_DESCRIPTOR ) break;
    if( (f & (O_EXCL|O_CREAT))==(O_EXCL|O_CREAT) ){
      (void)OS_UNLINK(z);
    }
    OS_CLOSE(fd);
    dbsql_log(DBSQL_WARNING,
                "attempt to open \"%s\" as file descriptor %d", z, fd);
    fd = -1;
    if( OS_OPEN("/dev/null", O_RDONLY, m)<0 ) break;
  }
  if( fd>=0 ){
    if( m!=0 ){
      struct stat statbuf;
      if( OS_FSTAT(fd, &statbuf)==0
       && statbuf.st_size==0
       && (statbuf.st_mode&0777)!=m
      ){
        OS_FCHMOD(fd, m);
      }
    }
#if defined(FD_CLOEXEC) && (!defined(O_CLOEXEC) || O_CLOEXEC==0)
    __os_fcntl(fd, F_SETFD, __os_fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
#endif
  }
  return fd;
}

/*
** Helper functions to obtain and relinquish the global mutex. The
** global mutex is used to protect the unix_inode_info objects used by
** this file, all of which may be shared by multiple threads.
**
** Function __unix_mutex_held() is used to assert() that the global mutex
** is held when required. This function is only used as part of assert()
** statements. e.g.
**
**   __unix_enter_mutex()
**     assert( __unix_mutex_held() );
**   unix_enter_leave()
**
** To prevent deadlock, the global __unix_big_lock must must be acquired
** before the unix_inode_info.lock_mutex mutex, if both are held.  It is
** OK to get the lock_mutex without holding __unix_big_lock first, but if
** that happens, the __unix_big_lock mutex must not be acquired until after
** lock_mutex is released.
**
**      OK:     enter(__unix_big_lock),  enter(lock_info)
**      OK:     enter(__unix_big_lock)
**      OK:     enter(lock_info)
**   ERROR:     enter(lock_info), enter(__unix_big_lock)
*/
static dbsql_mutex_t *__unix_big_lock = 0;
static void __unix_enter_mutex(void){
  assert( dbsql_mutex_notheld(__unix_big_lock) );  /* Not a recursive mutex */
  dbsql_mutex_enter(__unix_big_lock);
}
static void __unix_leave_mutex(void){
  assert( dbsql_mutex_held(__unix_big_lock) );
  dbsql_mutex_leave(__unix_big_lock);
}
#ifdef DBSQL_DEBUG
static int __unix_mutex_held(void) {
  return dbsql_mutex_held(__unix_big_lock);
}
#endif


#ifdef DBSQL_HAVE_OS_TRACE
/*
** Helper function for printing out trace information from debugging
** binaries. This returns the string representation of the supplied
** integer lock-type.
*/
static const char *__az_file_lock(int file_lock){
  switch( file_lock ){
    case NO_LOCK: return "NONE";
    case SHARED_LOCK: return "SHARED";
    case RESERVED_LOCK: return "RESERVED";
    case PENDING_LOCK: return "PENDING";
    case EXCLUSIVE_LOCK: return "EXCLUSIVE";
  }
  return "ERROR";
}
#endif

#ifdef DBSQL_LOCK_TRACE
/*
** Print out information about all locking operations.
**
** This routine is used for troubleshooting locks on multithreaded
** platforms.  Enable by compiling with the -DSQLITE_LOCK_TRACE
** command-line option on the compiler.  This code is normally
** turned off.
*/
static int __lock_trace(int fd, int op, struct flock *p){
  char *op_name, *type;
  int s;
  int saved_errno;
  if( op==F_GETLK ){
    op_name = "GETLK";
  }else if( op==F_SETLK ){
    op_name = "SETLK";
  }else{
    s = __os_fcntl(fd, op, p);
    __debug_printf("fcntl unknown %d %d %d\n", fd, op, s);
    return s;
  }
  if( p->l_type==F_RDLCK ){
    type = "RDLCK";
  }else if( p->l_type==F_WRLCK ){
    type = "WRLCK";
  }else if( p->l_type==F_UNLCK ){
    type = "UNLCK";
  }else{
    assert( 0 );
  }
  assert( p->l_whence==SEEK_SET );
  s = __os_fcntl(fd, op, p);
  saved_errno = errno;
  __debug_printf("fcntl %d %d %s %s %d %d %d %d\n",
     threadid, fd, op_name, type, (int)p->l_start, (int)p->l_len,
     (int)p->l_pid, s);
  if( s==(-1) && op==F_SETLK && (p->l_type==F_RDLCK || p->l_type==F_WRLCK) ){
    struct flock l2;
    l2 = *p;
    __os_fcntl(fd, F_GETLK, &l2);
    if( l2.l_type==F_RDLCK ){
      type = "RDLCK";
    }else if( l2.l_type==F_WRLCK ){
      type = "WRLCK";
    }else if( l2.l_type==F_UNLCK ){
      type = "UNLCK";
    }else{
      assert( 0 );
    }
    __debug_printf("fcntl-failure-reason: %s %d %d %d\n",
       type, (int)l2.l_start, (int)l2.l_len, (int)l2.l_pid);
  }
  errno = saved_errno;
  return s;
}
#undef __os_fcntl
#define __os_fcntl __lock_trace
#endif /* DBSQL_LOCK_TRACE */

/*
** Retry ftruncate() calls that fail due to EINTR
**
** All calls to ftruncate() within this file should be made through
** this wrapper.  On the Android platform, bypassing the logic below
** could lead to a corrupt database.
*/
static int robust_ftruncate(int h, int64_t sz){
  int rc;
#ifdef __ANDROID__
  /* On Android, ftruncate() always uses 32-bit offsets, even if
  ** _FILE_OFFSET_BITS=64 is defined. This means it is unsafe to attempt to
  ** truncate a file to any size larger than 2GiB. Silently ignore any
  ** such attempts.  */
  if( sz>(int64_t)0x7FFFFFFF ){
    rc = DBSQL_OK;
  }else
#endif
  do{ rc = OS_FTRUNCATE(h,sz); }while( rc<0 && errno==EINTR );
  return rc;
}

/*
** This routine translates a standard POSIX errno code into something
** useful to the clients of the DBSQL functions.  Specifically, it is
** intended to translate a variety of "try again" errors into DBSQL_BUSY
** and a variety of "please close the file descriptor NOW" errors into
** DBSQL_IOERR
**
** Errors during initialization of locks, or file system support for locks,
** should handle ENOLCK, ENOTSUP, EOPNOTSUPP separately.
*/
static int dbsql_error_from_posix_error(int posix_error, int dbsql_io_err) {
  assert( (dbsql_io_err == DBSQL_IOERR_LOCK) ||
          (dbsql_io_err == DBSQL_IOERR_UNLOCK) ||
          (dbsql_io_err == DBSQL_IOERR_RDLOCK) ||
          (dbsql_io_err == DBSQL_IOERR_CHECKRESERVEDLOCK) );
  switch (posix_error) {
  case EACCES:
  case EAGAIN:
  case ETIMEDOUT:
  case EBUSY:
  case EINTR:
  case ENOLCK:
    /* random NFS retry error, unless during file system support
     * introspection, in which it actually means what it says */
    return DBSQL_BUSY;
  
  case EPERM:
    return DBSQL_PERM;
  
  default:
    return dbsql_io_err;
  }
}


/******************************************************************************
****************** Begin Unique File ID Utility Used By __vx_works ***************
**
** On most versions of unix, we can get a unique ID for a file by concatenating
** the device number and the inode number.  But this does not work on __vx_works.
** On __vx_works, a unique file id must be based on the canonical filename.
**
** A pointer to an instance of the following structure can be used as a
** unique file ID in __vx_works.  Each instance of this structure contains
** a copy of the canonical filename.  There is also a reference count.
** The structure is reclaimed when the number of pointers to it drops to
** zero.
**
** There are never very many files open at one time and lookups are not
** a performance-critical path, so it is sufficient to put these
** structures on a linked list.
*/
struct vxworks_file_id {
  struct vxworks_file_id *next;  /* Next in a list of them all */
  int n_ref;                     /* Number of references to this one */
  int n_name;                    /* Length of the canonical_name[] string */
  char *canonical_name;         /* Canonical filename */
};

#if OS_VXWORKS
/*
** All unique filenames are held on a linked list headed by this
** variable:
*/
static struct vxworks_file_id *__vxworks_file_list = 0;
static dbsql_mutex_t *__vxworks_mutex = 0;

/*
** Simplify a filename into its canonical form
** by making the following changes:
**
**  * removing any trailing and duplicate /
**  * convert /./ into just /
**  * convert /A/../ where A is any simple name into just /
**
** Changes are made in-place.  Return the new name length.
**
** The original filename is in z[0..n-1].  Return the number of
** characters in the simplified name.
*/
static int __vxworks_simplify_name(char *z, int n){
  int i, j;
  while( n>1 && z[n-1]=='/' ){ n--; }
  for(i=j=0; i<n; i++){
    if( z[i]=='/' ){
      if( z[i+1]=='/' ) continue;
      if( z[i+1]=='.' && i+2<n && z[i+2]=='/' ){
        i += 1;
        continue;
      }
      if( z[i+1]=='.' && i+3<n && z[i+2]=='.' && z[i+3]=='/' ){
        while( j>0 && z[j-1]!='/' ){ j--; }
        if( j>0 ){ j--; }
        i += 2;
        continue;
      }
    }
    z[j++] = z[i];
  }
  z[j] = 0;
  return j;
}

/*
** Find a unique file ID for the given absolute pathname.  Return
** a pointer to the vxworks_file_id object.  This pointer is the unique
** file ID.
**
** The n_ref field of the vxworks_file_id object is incremented before
** the object is returned.  A new vxworks_file_id object is created
** and added to the global list if necessary.
**
** If a memory allocation error occurs, return NULL.
*/
static struct vxworks_file_id *__vxworks_find_file_id(const char *absolute_name){
  struct vxworks_file_id *p_new;         /* search key and new file ID */
  struct vxworks_file_id *candidate;   /* For looping over existing file IDs */
  int n;                              /* Length of absolute_name string */

  assert( absolute_name[0]=='/' );
  n = (int)strlen(absolute_name);
  p_new = dbsql_malloc64( sizeof(*p_new) + (n+1) );
  if( p_new==0 ) return 0;
  p_new->canonical_name = (char*)&p_new[1];
  memcpy(p_new->canonical_name, absolute_name, n+1);
  n = __vxworks_simplify_name(p_new->canonical_name, n);

  /* Search for an existing entry that matching the canonical name.
  ** If found, increment the reference count and return a pointer to
  ** the existing file ID.
  */
  dbsql_mutex_enter(__vxworks_mutex);
  for(candidate=__vxworks_file_list; candidate; candidate=candidate->next){
    if( candidate->n_name==n
     && memcmp(candidate->canonical_name, p_new->canonical_name, n)==0
    ){
       dbsql_free(p_new);
       candidate->n_ref++;
       dbsql_mutex_leave(__vxworks_mutex);
       return candidate;
    }
  }

  /* No match was found.  We will make a new file ID */
  p_new->n_ref = 1;
  p_new->n_name = n;
  p_new->next = __vxworks_file_list;
  __vxworks_file_list = p_new;
  dbsql_mutex_leave(__vxworks_mutex);
  return p_new;
}

/*
** Decrement the reference count on a vxworks_file_id object.  Free
** the object when the reference count reaches zero.
*/
static void __vxworks_release_file_id(struct vxworks_file_id *id){
  dbsql_mutex_enter(__vxworks_mutex);
  assert( id->n_ref>0 );
  id->n_ref--;
  if( id->n_ref==0 ){
    struct vxworks_file_id **pp;
    for(pp=&__vxworks_file_list; *pp && *pp!=id; pp = &((*pp)->next)){}
    assert( *pp==id );
    *pp = id->next;
    dbsql_free(id);
  }
  dbsql_mutex_leave(__vxworks_mutex);
}
#endif /* OS_VXWORKS */
/*************** End of Unique File ID Utility Used By __vx_works ****************
******************************************************************************/


/******************************************************************************
*************************** Posix Advisory Locking ****************************
**
** POSIX advisory locks are broken by design.  ANSI STD 1003.1 (1996)
** section 6.5.2.2 lines 483 through 490 specify that when a process
** sets or clears a lock, that operation overrides any prior locks set
** by the same process.  It does not explicitly say so, but this implies
** that it overrides locks set by the same process using a different
** file descriptor.  Consider this test case:
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** Suppose ./file1 and ./file2 are really the same file (because
** one is a hard or symbolic link to the other) then if you set
** an exclusive lock on fd1, then try to get an exclusive lock
** on fd2, it works.  I would have expected the second lock to
** fail since there was already a lock on the file due to fd1.
** But not so.  Since both locks came from the same process, the
** second overrides the first, even though they were on different
** file descriptors opened on different file names.
**
** This means that we cannot use POSIX locks to synchronize file access
** among competing threads of the same process.  POSIX locks will work fine
** to synchronize access for threads in separate processes, but not
** threads within the same process.
**
** To work around the problem, dbsql has to manage file locks internally
** on its own.  Whenever a new database is opened, we have to find the
** specific inode of the database file (the inode is determined by the
** st_dev and st_ino fields of the stat structure that fstat() fills in)
** and check for locks already existing on that inode.  When locks are
** created or removed, we have to look at our own internal record of the
** locks to see if another thread has previously set a lock on that same
** inode.
**
** (Aside: The use of inode numbers as unique IDs does not work on __vx_works.
** For __vx_works, we have to use the alternative unique ID system based on
** canonical filename and implemented in the previous division.)
**
** The dbsql_file_t structure for POSIX is no longer just an integer file
** descriptor.  It is now a structure that holds the integer file
** descriptor and a pointer to a structure that describes the internal
** locks on the corresponding inode.  There is one locking structure
** per inode, so if the same inode is opened twice, both unix_file structures
** point to the same locking structure.  The locking structure keeps
** a reference count (so we will know when to delete it) and a "cnt"
** field that tells us its internal lock status.  cnt==0 means the
** file is unlocked.  cnt==-1 means the file has an exclusive lock.
** cnt>0 means there are cnt shared locks on the file.
**
** Any attempt to lock or unlock a file first checks the locking
** structure.  The fcntl() system call is only invoked to set a
** POSIX lock if the internal lock structure transitions between
** a locked and an unlocked state.
**
** But wait:  there are yet more problems with POSIX advisory locks.
**
** If you close a file descriptor that points to a file that has locks,
** all locks on that file that are owned by the current process are
** released.  To work around this problem, each unix_inode_info object
** maintains a count of the number of pending locks on the inode.
** When an attempt is made to close an unix_file, if there are
** other unix_file open on the same inode that are holding locks, the call
** to close() the file descriptor is deferred until all of the locks clear.
** The unix_inode_info structure keeps a list of file descriptors that need to
** be closed and that list is walked (and cleared) when the last lock
** clears.
**
** Yet another problem:  __linux_threads do not play well with posix locks.
**
** Many older versions of linux use the __linux_threads library which is
** not posix compliant.  Under __linux_threads, a lock created by thread
** A cannot be modified or overridden by a different thread B.
** Only thread A can modify the lock.  Locking behavior is correct
** if the application uses the newer Native Posix dbsql_thread_t Library (NPTL)
** on linux - with NPTL a lock created by thread A can override locks
** in thread B.  But there is no way to know at compile-time which
** threading library is being used.  So there is no way to know at
** compile-time whether or not thread A can override locks on thread B.
** One has to do a run-time check to discover the behavior of the
** current process.
**
** dbsql used to support __linux_threads.  But support for __linux_threads
** was dropped beginning with version 3.7.0.  dbsql will still work with
** __linux_threads provided that (1) there is no more than one connection
** per database file in the same process and (2) database connections
** do not move across threads.
*/

/*
** An instance of the following structure serves as the key used
** to locate a particular unix_inode_info object.
*/
struct unix_file_id {
  dev_t dev;                  /* Device number */
#if OS_VXWORKS
  struct vxworks_file_id *id;  /* Unique file ID for vxworks. */
#else
  /* We are told that some versions of Android contain a bug that
  ** sizes ino_t at only 32-bits instead of 64-bits. (See
  ** https://android-review.googlesource.com/#/c/115351/3/dist/DBSQL.c)
  ** To work around this, always allocate 64-bits for the inode number.
  ** On small machines that only have 32-bit inodes, this wastes 4 bytes,
  ** but that should not be a big deal. */
  /* WAS:  ino_t ino;   */
  u_int64_t ino;                   /* Inode number */
#endif
};

/*
** An instance of the following structure is allocated for each open
** inode.
**
** A single inode can have multiple file descriptors, so each unix_file
** structure contains a pointer to an instance of this object and this
** object keeps a count of the number of unix_file pointing to it.
**
** Mutex rules:
**
**  (1) Only the lock_mutex mutex must be held in order to read or write
**      any of the locking fields:
**          n_shared, n_lock, file_lock, process_lock, unused
**
**  (2) When n_ref>0, then the following fields are unchanging and can
**      be read (but not written) without holding any mutex:
**          file_id, lock_mutex
**
**  (3) with_t the exceptions above, all the fields may only be read
**      or written while holding the global __unix_big_lock mutex.
**
** Deadlock prevention:  The global __unix_big_lock mutex may not
** be acquired while holding the lock_mutex mutex.  If both __unix_big_lock
** and lock_mutex are needed, then __unix_big_lock must be acquired first.
*/
struct unix_inode_info {
  struct unix_file_id file_id;       /* The lookup key */
  dbsql_mutex_t *lock_mutex;      /* Hold this mutex for... */
  int n_shared;                      /* Number of SHARED locks held */
  int n_lock;                        /* Number of outstanding file locks */
  unsigned char file_lock;          /* One of SHARED_LOCK, RESERVED_LOCK etc. */
  unsigned char process_lock;       /* An exclusive process lock is held */
  unix_unused_fd_t *unused;            /* Unused file descriptors to close */
  int n_ref;                       /* Number of pointers to this structure */
  unix_shm_node *shm_node;          /* Shared memory associated with this inode */
  unix_inode_info *next;           /* List of all unix_inode_info objects */
  unix_inode_info *prev;           /*    .... doubly linked */
#if DBSQL_ENABLE_LOCKING_STYLE
  unsigned long long shared_byte;  /* for AFP simulated shared lock */
#endif
#if OS_VXWORKS
  sem_t *sem;                    /* Named POSIX semaphore */
  char sem_names[MAX_PATHNAME+2];  /* Name of that semaphore */
#endif
};

/*
** A lists of all unix_inode_info objects.
**
** Must hold __unix_big_lock in order to read or write this variable.
*/
static unix_inode_info *__inode_list = 0;  /* All unix_inode_info objects */

#ifdef DBSQL_DEBUG
/*
** True if the inode mutex (on the unix_file.file_mutex field) is held, or not.
** This routine is used only within assert() to help verify correct mutex
** usage.
*/
int __unix_file_mutex_held(unix_file *file){
  assert( file->inode );
  return dbsql_mutex_held(file->inode->lock_mutex);
}
int __unix_file_mutex_notheld(unix_file *file){
  assert( file->inode );
  return dbsql_mutex_notheld(file->inode->lock_mutex);
}
#endif

/*
**
** This function - __unix_log_error_at_line(), is only ever called via the macro
** UNIX_LOG_ERROR().
**
** It is invoked after an error occurs in an OS function and errno has been
** set. It logs a message using dbsql_log() containing the current value of
** errno and, if possible, the human-readable equivalent from strerror() or
** strerror_r().
**
** The first argument passed to the macro should be the error code that
** will be returned to dbsql (e.g. DBSQL_IOERR_DELETE, DBSQL_CANTOPEN).
** The two subsequent arguments should be the name of the OS function that
** failed (e.g. "unlink", "open") and the associated file-system path,
** if any.
*/
#define UNIX_LOG_ERROR(a,b,c)     __unix_log_error_at_line(a,b,c,__LINE__)
static int __unix_log_error_at_line(
  int errcode,                    /* dbsql error code */
  const char *func,              /* Name of OS function that failed */
  const char *path,              /* File path associated with error */
  int i_line                       /* Source line number where error occurred */
){
  char *err;                     /* Message from strerror() or equivalent */
  int i_errno = errno;             /* Saved syscall error number */

  /* If this is not a threadsafe build (DBSQL_THREADSAFE==0), then use
  ** the strerror() function to obtain the human-readable error message
  ** equivalent to errno. Otherwise, use strerror_r().
  */
#if DBSQL_THREADSAFE && defined(HAVE_STRERROR_R)
  char errs[80];
  memset(errs, 0, sizeof(errs));
  err = errs;

  /* If STRERROR_R_CHAR_P (set by autoconf scripts) or __USE_GNU is defined,
  ** assume that the system provides the GNU version of strerror_r() that
  ** returns a pointer to a buffer containing the error message. That pointer
  ** may point to errs[], or it may point to some static storage somewhere.
  ** Otherwise, assume that the system provides the POSIX version of
  ** strerror_r(), which always writes an error message into errs[].
  **
  ** If the code incorrectly assumes that it is the POSIX version that is
  ** available, the error message will often be an empty string. Not a
  ** huge problem. Incorrectly concluding that the GNU version is available
  ** could lead to a segfault though.
  **
  ** Forum post 3f13857fa4062301 reports that the Android SDK may use
  ** int-type return, depending on its version.
  */
#if (defined(STRERROR_R_CHAR_P) || defined(__USE_GNU)) \
  && !defined(ANDROID) && !defined(__ANDROID__)
  err =
# endif
  strerror_r(i_errno, errs, sizeof(errs)-1);

#elif DBSQL_THREADSAFE
  /* This is a threadsafe build, but strerror_r() is not available. */
  err = "";
#else
  /* Non-threadsafe build, use strerror(). */
  err = strerror(i_errno);
#endif

  if( path==0 ) path = "";
  dbsql_log(errcode,
      "os_unix.c:%d: (%d) %s(%s) - %s",
      i_line, i_errno, func, path, err
  );

  return errcode;
}

/*
** Close a file descriptor.
**
** We assume that close() almost always works, since it is only in a
** very sick application or on a very sick platform that it might fail.
** If it does fail, simply leak the file descriptor, but do log the
** error.
**
** Note that it is not safe to retry close() after EINTR since the
** file descriptor might have already been reused by another thread.
** So we don't even try to recover from an EINTR.  Just log the error
** and move on.
*/
static void robust_close(unix_file *file, int h, int lineno){
  if( OS_CLOSE(h) ){
    __unix_log_error_at_line(DBSQL_IOERR_CLOSE, "close",
                       file ? file->path : 0, lineno);
  }
}

/*
** Set the file->last_errno.  Do this in a subroutine as that provides
** a convenient place to set a breakpoint.
*/
static void __store_last_errno(unix_file *file, int error){
  file->last_errno = error;
}

/*
** Close all file descriptors accumulated in the unix_inode_info->unused list.
*/
static void __close_pending_fds(unix_file *file){
  unix_inode_info *inode = file->inode;
  unix_unused_fd_t *p;
  unix_unused_fd_t *next;
  assert( __unix_file_mutex_held(file) );
  for(p=inode->unused; p; p=next){
    next = p->next;
    robust_close(file, p->fd, __LINE__);
    dbsql_free(p);
  }
  inode->unused = 0;
}

/*
** Release a unix_inode_info structure previously allocated by __find_inode_info().
**
** The global mutex must be held when this routine is called, but the mutex
** on the inode being deleted must NOT be held.
*/
static void __release_inode_info(unix_file *file){
  unix_inode_info *inode = file->inode;
  assert( __unix_mutex_held() );
  assert( __unix_file_mutex_notheld(file) );
  if( ALWAYS(inode) ){
    inode->n_ref--;
    if( inode->n_ref==0 ){
      assert( inode->shm_node==0 );
      dbsql_mutex_enter(inode->lock_mutex);
      __close_pending_fds(file);
      dbsql_mutex_leave(inode->lock_mutex);
      if( inode->prev ){
        assert( inode->prev->next==inode );
        inode->prev->next = inode->next;
      }else{
        assert( __inode_list==inode );
        __inode_list = inode->next;
      }
      if( inode->next ){
        assert( inode->next->prev==inode );
        inode->next->prev = inode->prev;
      }
      dbsql_mutex_free(inode->lock_mutex);
      dbsql_free(inode);
    }
  }
}

/*
** Given a file descriptor, locate the unix_inode_info object that
** describes that file descriptor.  Create a new one if necessary.  The
** return value might be uninitialized if an error occurs.
**
** The global mutex must held when calling this routine.
**
** Return an appropriate error code.
*/
static int __find_inode_info(
  unix_file *file,               /* Unix file with file desc used in the key */
  unix_inode_info **pp_inode        /* Return the unix_inode_info object here */
){
  int rc;                        /* System call return code */
  int fd;                        /* The file descriptor for file */
  struct unix_file_id file_id;      /* Lookup key for the unix_inode_info */
  struct stat statbuf;           /* Low-level file information */
  unix_inode_info *inode = 0;     /* Candidate unix_inode_info object */

  assert( __unix_mutex_held() );

  /* Get low-level information about the file that we can used to
  ** create a unique name for the file.
  */
  fd = file->h;
  rc = OS_FSTAT(fd, &statbuf);
  if( rc!=0 ){
    __store_last_errno(file, errno);
#if defined(EOVERFLOW) && defined(DBSQL_DISABLE_LFS)
    if( file->last_errno==EOVERFLOW ) return DBSQL_NOLFS;
#endif
    return DBSQL_IOERR;
  }

#ifdef __APPLE__
  /* On OS X on an msdos filesystem, the inode number is reported
  ** incorrectly for zero-size files.  See ticket #3260.  To work
  ** around this problem (we consider it a bug in OS X, not dbsql)
  ** we always increase the file size to 1 by writing a single byte
  ** prior to accessing the inode number.  The one byte written is
  ** an ASCII 'S' character which also happens to be the first byte
  ** in the header of every dbsql database.  In this way, if there
  ** is a race condition such that another thread has already populated
  ** the first page of the database, no damage is done.
  */
  if( statbuf.st_size==0 && (file->fs_flags & DBSQL_FSFLAGS_IS_MSDOS)!=0 ){
    do{ rc = OS_WRITE(fd, "S", 1); }while( rc<0 && errno==EINTR );
    if( rc!=1 ){
      __store_last_errno(file, errno);
      return DBSQL_IOERR;
    }
    if( fsync(fd) ){
      __store_last_errno(file, errno);
      return DBSQL_IOERR_FSYNC;
    }
    rc = OS_FSTAT(fd, &statbuf);
    if( rc!=0 ){
      __store_last_errno(file, errno);
      return DBSQL_IOERR;
    }
  }
#endif

  memset(&file_id, 0, sizeof(file_id));
  file_id.dev = statbuf.st_dev;
#if OS_VXWORKS
  file_id.id = file->id;
#else
  file_id.ino = (u_int64_t)statbuf.st_ino;
#endif
  assert( __unix_mutex_held() );
  inode = __inode_list;
  while( inode && memcmp(&file_id, &inode->file_id, sizeof(file_id)) ){
    inode = inode->next;
  }
  if( inode==0 ){
    inode = dbsql_malloc64( sizeof(*inode) );
    if( inode==0 ){
      return DBSQL_NOMEM_BKPT;
    }
    memset(inode, 0, sizeof(*inode));
    memcpy(&inode->file_id, &file_id, sizeof(file_id));
    if( __global_config.core_mutex ){
      inode->lock_mutex = dbsql_mutex_alloc(DBSQL_MUTEX_FAST);
      if( inode->lock_mutex==0 ){
        dbsql_free(inode);
        return DBSQL_NOMEM_BKPT;
      }
    }
    inode->n_ref = 1;
    assert( __unix_mutex_held() );
    inode->next = __inode_list;
    inode->prev = 0;
    if( __inode_list ) __inode_list->prev = inode;
    __inode_list = inode;
  }else{
    inode->n_ref++;
  }
  *pp_inode = inode;
  return DBSQL_OK;
}

/*
** Return TRUE if file has been renamed or unlinked since it was first opened.
*/
static int __file_has_moved(unix_file *file){
#if OS_VXWORKS
  return file->inode!=0 && file->id!=file->inode->file_id.id;
#else
  struct stat buf;
  return file->inode!=0 &&
      (OS_STAT(file->path, &buf)!=0
         || (u_int64_t)buf.st_ino!=file->inode->file_id.ino);
#endif
}


/*
** Check a unix_file that is a database.  Verify the following:
**
** (1) There is exactly one hard link on the file
** (2) The file is not a symbolic link
** (3) The file has not been renamed or unlinked
**
** Issue dbsql_log(DBSQL_WARNING,...) messages if anything is not right.
*/
static void __verify_db_file(unix_file *file){
  struct stat buf;
  int rc;

  /* These verifications occurs for the main database only */
  if( file->ctrl_flags & UNIXFILE_NOLOCK ) return;

  rc = OS_FSTAT(file->h, &buf);
  if( rc!=0 ){
    dbsql_log(DBSQL_WARNING, "cannot fstat db file %s", file->path);
    return;
  }
  if( buf.st_nlink==0 ){
    dbsql_log(DBSQL_WARNING, "file unlinked while open: %s", file->path);
    return;
  }
  if( buf.st_nlink>1 ){
    dbsql_log(DBSQL_WARNING, "multiple links to file: %s", file->path);
    return;
  }
  if( __file_has_moved(file) ){
    dbsql_log(DBSQL_WARNING, "file renamed while open: %s", file->path);
    return;
  }
}


/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *res_out
** to a non-zero value otherwise *res_out is set to zero.  The return value
** is set to DBSQL_OK unless an I/O error occurs during lock checking.
*/
static int __unix_check_reserved_lock(dbsql_file_t *id, int *res_out){
  int rc = DBSQL_OK;
  int reserved = 0;
  unix_file *file = (unix_file*)id;

  SIMULATE_IO_ERROR( return DBSQL_IOERR_CHECKRESERVEDLOCK; );

  assert( file );
  assert( file->file_lock<=SHARED_LOCK );
  dbsql_mutex_enter(file->inode->lock_mutex);

  /* Check if a thread in this process holds such a lock */
  if( file->inode->file_lock>SHARED_LOCK ){
    reserved = 1;
  }

  /* Otherwise see if some other process holds it.
  */
#ifndef __DJGPP__
  if( !reserved && !file->inode->process_lock ){
    struct flock lock;
    lock.l_whence = SEEK_SET;
    lock.l_start = RESERVED_BYTE;
    lock.l_len = 1;
    lock.l_type = F_WRLCK;
    if( __os_fcntl(file->h, F_GETLK, &lock) ){
      rc = DBSQL_IOERR_CHECKRESERVEDLOCK;
      __store_last_errno(file, errno);
    } else if( lock.l_type!=F_UNLCK ){
      reserved = 1;
    }
  }
#endif

  dbsql_mutex_leave(file->inode->lock_mutex);
  OSTRACE(("TEST WR-LOCK %d %d %d (unix)\n", file->h, rc, reserved));

  *res_out = reserved;
  return rc;
}

/* Forward declaration*/
static int __unix_sleep(dbsql_vfs_t*,int);

/*
** Set a posix-advisory-lock.
**
** There are two versions of this routine.  If compiled with
** DBSQL_ENABLE_SETLK_TIMEOUT then the routine has an extra parameter
** which is a pointer to a unix_file.  If the unix_file->i_busy_timeout
** value is set, then it is the number of milliseconds to wait before
** failing the lock.  The i_busy_timeout value is always reset back to
** zero on each call.
**
** If DBSQL_ENABLE_SETLK_TIMEOUT is not defined, then do a non-blocking
** attempt to set the lock.
*/
#ifndef DBSQL_ENABLE_SETLK_TIMEOUT
# define OS_SET_POSIX_ADVISORY_LOCK(h,x,t) __os_fcntl(h,F_SETLK,x)
#else
static int OS_SET_POSIX_ADVISORY_LOCK(
  int h,                /* The file descriptor on which to take the lock */
  struct flock *lock,  /* The description of the lock */
  unix_file *file       /* Structure holding timeout value */
){
  int rc = 0;

  if( file->i_busy_timeout==0 ){
    /* unix_file->i_busy_timeout is set to 0. In this case, attempt a 
    ** non-blocking lock. */
    rc = __os_fcntl(h,F_SETLK,lock);
  }else{
    /* unix_file->i_busy_timeout is set to greater than zero. In this case,
    ** attempt a blocking-lock with a unix_file->i_busy_timeout ms timeout.
    **
    ** On systems that support some kind of blocking file lock operation,
    ** this block should be replaced by code to attempt a blocking lock
    ** with a timeout of unix_file->i_busy_timeout ms. The code below is 
    ** placeholder code. If DBSQL_TEST is defined, the placeholder code
    ** retries the lock once every 1ms until it succeeds or the timeout
    ** is reached. Or, if DBSQL_TEST is not defined, the placeholder
    ** code attempts a non-blocking lock and sets unix_file->i_busy_timeout
    ** to 0. This causes the caller to return DBSQL_BUSY, instead of
    ** DBSQL_BUSY_TIMEOUT to dbsql - as required by a VFS that does not 
    ** support blocking locks.
    */
#ifdef DBSQL_TEST
    int tm = file->i_busy_timeout;
    while( tm>0 ){
      rc = __os_fcntl(h,F_SETLK,lock);
      if( rc==0 ) break;
      __unix_sleep(0,1000);
      tm--;
    }
#else
    rc = __os_fcntl(h,F_SETLK,lock);
    file->i_busy_timeout = 0;
#endif
    /* End of code to replace with real blocking-locks code. */
  }

  return rc;
}
#endif /* DBSQL_ENABLE_SETLK_TIMEOUT */


/*
** Attempt to set a system-lock on the file file.  The lock is
** described by lock.
**
** If the file was opened read/write from unix-excl, then the only lock
** ever obtained is an exclusive lock, and it is obtained exactly once
** the first time any lock is attempted.  All subsequent system locking
** operations become no-ops.  Locking operations still happen internally,
** in order to coordinate access between separate database connections
** within this process, but all of that is handled in memory and the
** operating system does not participate.
**
** This function is a pass-through to fcntl(F_SETLK) if file is using
** any VFS other than "unix-excl" or if file is opened on "unix-excl"
** and is read-only.
**
** Zero is returned if the call completes successfully, or -1 if a call
** to fcntl() fails. In this case, errno is set appropriately (by fcntl()).
*/
static int __unix_file_lock(unix_file *file, struct flock *p_lock){
  int rc;
  unix_inode_info *inode = file->inode;
  assert( inode!=0 );
  assert( dbsql_mutex_held(inode->lock_mutex) );
  if( (file->ctrl_flags & (UNIXFILE_EXCL|UNIXFILE_RDONLY))==UNIXFILE_EXCL ){
    if( inode->process_lock==0 ){
      struct flock lock;
      /* assert( inode->n_lock==0 ); <-- Not true if unix-excl READONLY used */
      lock.l_whence = SEEK_SET;
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
      lock.l_type = F_WRLCK;
      rc = OS_SET_POSIX_ADVISORY_LOCK(file->h, &lock, file);
      if( rc<0 ) return rc;
      inode->process_lock = 1;
      inode->n_lock++;
    }else{
      rc = 0;
    }
  }else{
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    if( file->block_on_connect && p_lock->l_type==F_RDLCK
     && p_lock->l_start==SHARED_FIRST && p_lock->l_len==SHARED_SIZE
    ){
      rc = __os_fcntl(file->h, F_SETLKW, p_lock);
    }else
#endif
    rc = OS_SET_POSIX_ADVISORY_LOCK(file->h, p_lock, file);
  }
  return rc;
}

#if !defined(DBSQL_WASI) && !defined(DBSQL_OMIT_WAL)
/* Forward reference */
static int UNIX_IS_SHARING_SHM_NODE(unix_file*);
#else
#define UNIX_IS_SHARING_SHM_NODE(file) (0)
#endif

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
*/
static int __unix_lock(dbsql_file_t *id, int file_lock){
  /* The following describes the implementation of the various locks and
  ** lock transitions in terms of the POSIX advisory shared and exclusive
  ** lock primitives (called read-locks and write-locks below, to avoid
  ** confusion with dbsql lock names). The algorithms are complicated
  ** slightly in order to be compatible with Windows95 systems simultaneously
  ** accessing the same database file, in case that is ever required.
  **
  ** Symbols defined in os.h identify the 'pending byte' and the 'reserved
  ** byte', each single bytes at well known offsets, and the 'shared byte
  ** range', a range of 510 bytes at a well known offset.
  **
  ** To obtain a SHARED lock, a read-lock is obtained on the 'pending
  ** byte'.  If this is successful, 'shared byte range' is read-locked
  ** and the lock on the 'pending byte' released.  (Legacy note:  When
  ** dbsql was first developed, Windows95 systems were still very common,
  ** and Windows95 lacks a shared-lock capability.  So on Windows95, a
  ** single randomly selected by from the 'shared byte range' is locked.
  ** Windows95 is now pretty much extinct, but this work-around for the
  ** lack of shared-locks on Windows95 lives on, for backwards
  ** compatibility.)
  **
  ** A process may only obtain a RESERVED lock after it has a SHARED lock.
  ** A RESERVED lock is implemented by grabbing a write-lock on the
  ** 'reserved byte'.
  **
  ** An EXCLUSIVE lock may only be requested after either a SHARED or
  ** RESERVED lock is held. An EXCLUSIVE lock is implemented by obtaining
  ** a write-lock on the entire 'shared byte range'. Since all other locks
  ** require a read-lock on one of the bytes within this range, this ensures
  ** that no other locks are held on the database.
  **
  ** If a process that holds a RESERVED lock requests an EXCLUSIVE, then
  ** a PENDING lock is obtained first. A PENDING lock is implemented by
  ** obtaining a write-lock on the 'pending byte'. This ensures that no new
  ** SHARED locks can be obtained, but existing SHARED locks are allowed to
  ** persist. If the call to this function fails to obtain the EXCLUSIVE
  ** lock in this case, it holds the PENDING lock instead. The client may
  ** then re-attempt the EXCLUSIVE lock later on, after existing SHARED
  ** locks have cleared.
  */
  int rc = DBSQL_OK;
  unix_file *file = (unix_file*)id;
  unix_inode_info *inode;
  struct flock lock;
  int t_errno = 0;

  assert( file );
  OSTRACE(("LOCK    %d %s was %s(%s,%d) pid=%d (unix)\n", file->h,
      __az_file_lock(file_lock), __az_file_lock(file->file_lock),
      __az_file_lock(file->inode->file_lock), file->inode->n_shared,
      OS_GETPID(0)));

  /* If there is already a lock of this type or more restrictive on the
  ** unix_file, do nothing. Don't use the end_lock: exit path, as
  ** __unix_enter_mutex() hasn't been called yet.
  */
  if( file->file_lock>=file_lock ){
    OSTRACE(("LOCK    %d %s ok (already held) (unix)\n", file->h,
            __az_file_lock(file_lock)));
    return DBSQL_OK;
  }

  /* Make sure the locking sequence is correct.
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) dbsql never explicitly requests a pending lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  */
  assert( file->file_lock!=NO_LOCK || file_lock==SHARED_LOCK );
  assert( file_lock!=PENDING_LOCK );
  assert( file_lock!=RESERVED_LOCK || file->file_lock==SHARED_LOCK );

  /* This mutex is needed because file->inode is shared across threads
  */
  inode = file->inode;
  dbsql_mutex_enter(inode->lock_mutex);

  /* If some thread using this PID has a lock via a different unix_file*
  ** handle that precludes the requested lock, return BUSY.
  */
  if( (file->file_lock!=inode->file_lock &&
          (inode->file_lock>=PENDING_LOCK || file_lock>SHARED_LOCK))
  ){
    rc = DBSQL_BUSY;
    goto end_lock;
  }

  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return DBSQL_OK.
  */
  if( file_lock==SHARED_LOCK &&
      (inode->file_lock==SHARED_LOCK || inode->file_lock==RESERVED_LOCK) ){
    assert( file_lock==SHARED_LOCK );
    assert( file->file_lock==0 );
    assert( inode->n_shared>0 );
    file->file_lock = SHARED_LOCK;
    inode->n_shared++;
    inode->n_lock++;
    goto end_lock;
  }


  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  */
  lock.l_len = 1L;
  lock.l_whence = SEEK_SET;
  if( file_lock==SHARED_LOCK
   || (file_lock==EXCLUSIVE_LOCK && file->file_lock==RESERVED_LOCK)
  ){
    lock.l_type = (file_lock==SHARED_LOCK?F_RDLCK:F_WRLCK);
    lock.l_start = PENDING_BYTE;
    if( __unix_file_lock(file, &lock) ){
      t_errno = errno;
      rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_LOCK);
      if( rc!=DBSQL_BUSY ){
        __store_last_errno(file, t_errno);
      }
      goto end_lock;
    }else if( file_lock==EXCLUSIVE_LOCK ){
      file->file_lock = PENDING_LOCK;
      inode->file_lock = PENDING_LOCK;
    }
  }


  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  */
  if( file_lock==SHARED_LOCK ){
    assert( inode->n_shared==0 );
    assert( inode->file_lock==0 );
    assert( rc==DBSQL_OK );

    /* Now get the read-lock */
    lock.l_start = SHARED_FIRST;
    lock.l_len = SHARED_SIZE;
    if( __unix_file_lock(file, &lock) ){
      t_errno = errno;
      rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_LOCK);
    }

    /* Drop the temporary PENDING lock */
    lock.l_start = PENDING_BYTE;
    lock.l_len = 1L;
    lock.l_type = F_UNLCK;
    if( __unix_file_lock(file, &lock) && rc==DBSQL_OK ){
      /* This could happen with a network mount */
      t_errno = errno;
      rc = DBSQL_IOERR_UNLOCK;
    }

    if( rc ){
      if( rc!=DBSQL_BUSY ){
        __store_last_errno(file, t_errno);
      }
      goto end_lock;
    }else{
      file->file_lock = SHARED_LOCK;
      inode->n_lock++;
      inode->n_shared = 1;
    }
  }else if( file_lock==EXCLUSIVE_LOCK && inode->n_shared>1 ){
    /* We are trying for an exclusive lock but another thread in this
    ** same process is still holding a shared lock. */
    rc = DBSQL_BUSY;
  }else if( UNIX_IS_SHARING_SHM_NODE(file) ){
    /* We are in WAL mode and attempting to delete the SHM and WAL
    ** files due to closing the connection or changing out of WAL mode,
    ** but another process still holds locks on the SHM file, thus
    ** indicating that database locks have been broken, perhaps due
    ** to a rogue close(open(db_file)) or similar.
    */
    rc = DBSQL_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    */
    assert( 0!=file->file_lock );
    lock.l_type = F_WRLCK;

    assert( file_lock==RESERVED_LOCK || file_lock==EXCLUSIVE_LOCK );
    if( file_lock==RESERVED_LOCK ){
      lock.l_start = RESERVED_BYTE;
      lock.l_len = 1L;
    }else{
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
    }

    if( __unix_file_lock(file, &lock) ){
      t_errno = errno;
      rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_LOCK);
      if( rc!=DBSQL_BUSY ){
        __store_last_errno(file, t_errno);
      }
    }
  }


#ifdef DBSQL_DEBUG
  /* Set up the transaction-counter change checking flags when
  ** transitioning from a SHARED to a RESERVED lock.  The change
  ** from SHARED to RESERVED marks the beginning of a normal
  ** write operation (not a hot journal rollback).
  */
  if( rc==DBSQL_OK
   && file->file_lock<=SHARED_LOCK
   && file_lock==RESERVED_LOCK
  ){
    file->trans_cntr_chng = 0;
    file->db_update = 0;
    file->in_normal_write = 1;
  }
#endif

  if( rc==DBSQL_OK ){
    file->file_lock = file_lock;
    inode->file_lock = file_lock;
  }

end_lock:
  dbsql_mutex_leave(inode->lock_mutex);
  OSTRACE(("LOCK    %d %s %s (unix)\n", file->h, __az_file_lock(file_lock),
      rc==DBSQL_OK ? "ok" : "failed"));
  return rc;
}

/*
** Add the file descriptor used by file handle file to the corresponding
** unused list.
*/
static void __set_pending_fd(unix_file *file){
  unix_inode_info *inode = file->inode;
  unix_unused_fd_t *p = file->preallocated_unused;
  assert( __unix_file_mutex_held(file) );
  p->next = inode->unused;
  inode->unused = p;
  file->h = -1;
  file->preallocated_unused = 0;
}

/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
**
** If handle_nfs_unlock is true, then on downgrading an EXCLUSIVE_LOCK to SHARED
** the byte range is divided into 2 parts and the first part is unlocked then
** set to a read lock, then the other part is simply unlocked.  This works
** around a bug in BSD NFS lockd (also seen on __mac_osx 10.3+) that fails to
** remove the write lock on a region when a read lock is set.
*/
static int __posix_unlock(dbsql_file_t *id, int file_lock, int handle_nfs_unlock){
  unix_file *file = (unix_file*)id;
  unix_inode_info *inode;
  struct flock lock;
  int rc = DBSQL_OK;

  assert( file );
  OSTRACE(("UNLOCK  %d %d was %d(%d,%d) pid=%d (unix)\n", file->h, file_lock,
      file->file_lock, file->inode->file_lock, file->inode->n_shared,
      OS_GETPID(0)));

  assert( file_lock<=SHARED_LOCK );
  if( file->file_lock<=file_lock ){
    return DBSQL_OK;
  }
  inode = file->inode;
  dbsql_mutex_enter(inode->lock_mutex);
  assert( inode->n_shared!=0 );
  if( file->file_lock>SHARED_LOCK ){
    assert( inode->file_lock==file->file_lock );

#ifdef DBSQL_DEBUG
    /* When reducing a lock such that other processes can start
    ** reading the database file again, make sure that the
    ** transaction counter was updated if any part of the database
    ** file changed.  If the transaction counter is not updated,
    ** other connections to the same file might not realize that
    ** the file has changed and hence might not know to flush their
    ** cache.  The use of a stale cache can lead to database corruption.
    */
    file->in_normal_write = 0;
#endif

    /* downgrading to a shared lock on NFS involves clearing the write lock
    ** before establishing the readlock - to avoid a race condition we downgrade
    ** the lock in 2 blocks, so that part of the range will be covered by a
    ** write lock until the rest is covered by a read lock:
    **  1:   [WWWWW]
    **  2:   [....W]
    **  3:   [RRRRW]
    **  4:   [RRRR.]
    */
    if( file_lock==SHARED_LOCK ){
#if !defined(__APPLE__) || !DBSQL_ENABLE_LOCKING_STYLE
      (void)handle_nfs_unlock;
      assert( handle_nfs_unlock==0 );
#endif
#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
      if( handle_nfs_unlock ){
        int t_errno;               /* Error code from system call errors */
        off_t div_size = SHARED_SIZE - 1;
      
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = div_size;
        if( __unix_file_lock(file, &lock)==(-1) ){
          t_errno = errno;
          rc = DBSQL_IOERR_UNLOCK;
          __store_last_errno(file, t_errno);
          goto end_unlock;
        }
        lock.l_type = F_RDLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = div_size;
        if( __unix_file_lock(file, &lock)==(-1) ){
          t_errno = errno;
          rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_RDLOCK);
          if( IS_LOCK_ERROR(rc) ){
            __store_last_errno(file, t_errno);
          }
          goto end_unlock;
        }
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST+div_size;
        lock.l_len = SHARED_SIZE-div_size;
        if( __unix_file_lock(file, &lock)==(-1) ){
          t_errno = errno;
          rc = DBSQL_IOERR_UNLOCK;
          __store_last_errno(file, t_errno);
          goto end_unlock;
        }
      }else
#endif /* defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE */
      {
        lock.l_type = F_RDLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = SHARED_SIZE;
        if( __unix_file_lock(file, &lock) ){
          /* In theory, the call to __unix_file_lock() cannot fail because another
          ** process is holding an incompatible lock. If it does, this
          ** indicates that the other process is not following the locking
          ** protocol. If this happens, return DBSQL_IOERR_RDLOCK. returning_t
          ** DBSQL_BUSY would confuse the upper layer (in practice it causes
          ** an assert to fail). */
          rc = DBSQL_IOERR_RDLOCK;
          __store_last_errno(file, errno);
          goto end_unlock;
        }
      }
    }
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = PENDING_BYTE;
    lock.l_len = 2L;  assert( PENDING_BYTE+1==RESERVED_BYTE );
    if( __unix_file_lock(file, &lock)==0 ){
      inode->file_lock = SHARED_LOCK;
    }else{
      rc = DBSQL_IOERR_UNLOCK;
      __store_last_errno(file, errno);
      goto end_unlock;
    }
  }
  if( file_lock==NO_LOCK ){
    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    */
    inode->n_shared--;
    if( inode->n_shared==0 ){
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = lock.l_len = 0L;
      if( __unix_file_lock(file, &lock)==0 ){
        inode->file_lock = NO_LOCK;
      }else{
        rc = DBSQL_IOERR_UNLOCK;
        __store_last_errno(file, errno);
        inode->file_lock = NO_LOCK;
        file->file_lock = NO_LOCK;
      }
    }

    /* Decrement the count of locks against this same file.  When the
    ** count reaches zero, close any other file descriptors whose close
    ** was deferred because of outstanding locks.
    */
    inode->n_lock--;
    assert( inode->n_lock>=0 );
    if( inode->n_lock==0 ) __close_pending_fds(file);
  }

end_unlock:
  dbsql_mutex_leave(inode->lock_mutex);
  if( rc==DBSQL_OK ){
    file->file_lock = file_lock;
  }
  return rc;
}

/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int __unix_unlock(dbsql_file_t *id, int file_lock){
#if DBSQL_MAX_MMAP_SIZE>0
  assert( file_lock==SHARED_LOCK || ((unix_file *)id)->n_fetch_out==0 );
#endif
  return __posix_unlock(id, file_lock, 0);
}

#if DBSQL_MAX_MMAP_SIZE>0
static int __unix_mapfile(unix_file *fd, int64_t n_byte);
static void __unix_unmapfile(unix_file *fd);
#endif

/*
** This function performs the parts of the "close file" operation
** common to all locking schemes. It closes the directory and file
** handles, if they are valid, and sets all fields of the unix_file
** structure to 0.
**
** It is *not* necessary to hold the mutex when this routine is called,
** even on __vx_works.  A mutex will be acquired on __vx_works by the
** __vxworks_release_file_id() routine.
*/
static int __close_unix_file(dbsql_file_t *id){
  unix_file *file = (unix_file*)id;
#if DBSQL_MAX_MMAP_SIZE>0
  __unix_unmapfile(file);
#endif
  if( file->h>=0 ){
    robust_close(file, file->h, __LINE__);
    file->h = -1;
  }
#if OS_VXWORKS
  if( file->id ){
    if( file->ctrl_flags & UNIXFILE_DELETE ){
      OS_UNLINK(file->id->canonical_name);
    }
    __vxworks_release_file_id(file->id);
    file->id = 0;
  }
#endif
#ifdef DBSQL_UNLINK_AFTER_CLOSE
  if( file->ctrl_flags & UNIXFILE_DELETE ){
    OS_UNLINK(file->path);
    dbsql_free(*(char**)&file->path);
    file->path = 0;
  }
#endif
  OSTRACE(("CLOSE   %-3d\n", file->h));
  OPEN_COUNTER(-1);
  dbsql_free(file->preallocated_unused);
  memset(file, 0, sizeof(unix_file));
  return DBSQL_OK;
}

/*
** Close a file.
*/
static int __unix_close(dbsql_file_t *id){
  int rc = DBSQL_OK;
  unix_file *file = (unix_file *)id;
  unix_inode_info *inode = file->inode;

  assert( inode!=0 );
  __verify_db_file(file);
  __unix_unlock(id, NO_LOCK);
  assert( __unix_file_mutex_notheld(file) );
  __unix_enter_mutex();

  /* unix_file.inode is always valid here. Otherwise, a different close
  ** routine (e.g. __nolock_close()) would be called instead.
  */
  assert( file->inode->n_lock>0 || file->inode->process_lock==0 );
  dbsql_mutex_enter(inode->lock_mutex);
  if( inode->n_lock ){
    /* If there are outstanding locks, do not actually close the file just
    ** yet because that would clear those locks.  Instead, add the file
    ** descriptor to inode->unused list.  It will be automatically closed
    ** when the last lock is cleared.
    */
    __set_pending_fd(file);
  }
  dbsql_mutex_leave(inode->lock_mutex);
  __release_inode_info(file);
  assert( file->shm==0 );
  rc = __close_unix_file(id);
  __unix_leave_mutex();
  return rc;
}

/************** End of the posix advisory lock implementation *****************
******************************************************************************/

/******************************************************************************
****************************** No-op Locking **********************************
**
** Of the various locking implementations available, this is by far the
** simplest:  locking is ignored.  No attempt is made to lock the database
** file for reading or writing.
**
** This locking mode is appropriate for use on read-only databases
** (ex: databases that are burned into CD-ROM, for example.)  It can
** also be used if the application employs some external mechanism to
** prevent simultaneous access of the same database by two or more
** database connections.  But there is a serious risk of database
** corruption if this locking mode is used in situations where multiple
** database connections are accessing the same database file at the same
** time and one or more of those connections are writing.
*/

static int __nolock_check_reserved_lock(dbsql_file_t *__not_used, int *res_out){
  UNUSED_PARAMETER(__not_used);
  *res_out = 0;
  return DBSQL_OK;
}
static int __nolock_lock(dbsql_file_t *__not_used, int __not_used2){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  return DBSQL_OK;
}
static int __nolock_unlock(dbsql_file_t *__not_used, int __not_used2){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  return DBSQL_OK;
}

/*
** Close the file.
*/
static int __nolock_close(dbsql_file_t *id) {
  return __close_unix_file(id);
}

/******************* End of the no-op lock implementation *********************
******************************************************************************/

/******************************************************************************
************************* Begin dot-file Locking ******************************
**
** The dotfile locking implementation uses the existence of separate lock
** files (really a directory) to control access to the database.  This works
** on just about every filesystem imaginable.  But there are serious downsides:
**
**    (1)  There is zero concurrency.  A single reader blocks all other
**         connections from reading or writing the database.
**
**    (2)  An application crash or power loss can leave stale lock files
**         sitting around that need to be cleared manually.
**
** Nevertheless, a dotlock is an appropriate locking mode for use if no
** other locking strategy is available.
**
** Dotfile locking works by creating a subdirectory in the same directory as
** the database and with the same name but with a ".lock" extension added.
** The existence of a lock directory implies an EXCLUSIVE lock.  All other
** lock types (SHARED, RESERVED, PENDING) are mapped into EXCLUSIVE.
*/

/*
** The file suffix added to the data base filename in order to create the
** lock directory.
*/
#define DOTLOCK_SUFFIX ".lock"

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If the caller holds a SHARED
** or greater lock when it is called, then it is assumed that no other
** client may hold RESERVED. Or, if the caller holds no lock, then it
** is assumed another client holds RESERVED if the lock-file exists.
*/
static int __dotlock_check_reserved_lock(dbsql_file_t *id, int *res_out) {
  unix_file *file = (unix_file*)id;
  SIMULATE_IO_ERROR( return DBSQL_IOERR_CHECKRESERVEDLOCK; );

  if( file->file_lock>=SHARED_LOCK ){
    *res_out = 0;
  }else{
    *res_out = OS_ACCESS((const char*)file->locking_context, 0)==0;
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (dotlock)\n", file->h, 0, *res_out));
  return DBSQL_OK;
}

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
**
** with_t dotfile locking, we really only support state (4): EXCLUSIVE.
** But we track the other locking levels internally.
*/
static int __dotlock_lock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  char *lock_file = (char *)file->locking_context;
  int rc = DBSQL_OK;


  /* If we have any lock, then the lock file already exists.  All we have
  ** to do is adjust our internal record of the lock level.
  */
  if( file->file_lock > NO_LOCK ){
    file->file_lock = file_lock;
    /* Always update the timestamp on the old file */
#ifdef HAVE_UTIME
    utime(lock_file, NULL);
#else
    utimes(lock_file, NULL);
#endif
    return DBSQL_OK;
  }

  /* grab an exclusive lock */
  rc = OS_MKDIR(lock_file, 0777);
  if( rc<0 ){
    /* failed to open/create the lock directory */
    int t_errno = errno;
    if( EEXIST == t_errno ){
      rc = DBSQL_BUSY;
    } else {
      rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_LOCK);
      if( rc!=DBSQL_BUSY ){
        __store_last_errno(file, t_errno);
      }
    }
    return rc;
  }

  /* got it, set the type and return ok */
  file->file_lock = file_lock;
  return rc;
}

/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
**
** When the locking level reaches NO_LOCK, delete the lock file.
*/
static int __dotlock_unlock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  char *lock_file = (char *)file->locking_context;
  int rc;

  assert( file );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (dotlock)\n", file->h, file_lock,
           file->file_lock, OS_GETPID(0)));
  assert( file_lock<=SHARED_LOCK );

  /* no-op if possible */
  if( file->file_lock==file_lock ){
    return DBSQL_OK;
  }

  /* To downgrade to shared, simply update our internal notion of the
  ** lock state.  No need to mess with the file on disk.
  */
  if( file_lock==SHARED_LOCK ){
    file->file_lock = SHARED_LOCK;
    return DBSQL_OK;
  }

  /* To fully unlock the database, delete the lock file */
  assert( file_lock==NO_LOCK );
  rc = OS_RMDIR(lock_file);
  if( rc<0 ){
    int t_errno = errno;
    if( t_errno==ENOENT ){
      rc = DBSQL_OK;
    }else{
      rc = DBSQL_IOERR_UNLOCK;
      __store_last_errno(file, t_errno);
    }
    return rc;
  }
  file->file_lock = NO_LOCK;
  return DBSQL_OK;
}

/*
** Close a file.  Make sure the lock has been released before closing.
*/
static int __dotlock_close(dbsql_file_t *id) {
  unix_file *file = (unix_file*)id;
  assert( id!=0 );
  __dotlock_unlock(id, NO_LOCK);
  dbsql_free(file->locking_context);
  return __close_unix_file(id);
}
/****************** End of the dot-file lock implementation *******************
******************************************************************************/

/******************************************************************************
************************** Begin flock Locking ********************************
**
** Use the flock() system call to do file locking.
**
** flock() locking is like dot-file locking in that the various
** fine-grain locking levels supported by dbsql are collapsed into
** a single exclusive lock.  In other words, SHARED, RESERVED, and
** PENDING locks are the same thing as an EXCLUSIVE lock.  dbsql
** still works when you do this, but concurrency is reduced since
** only a single process can be reading the database at a time.
**
** Omit this section if DBSQL_ENABLE_LOCKING_STYLE is turned off
*/
#if DBSQL_ENABLE_LOCKING_STYLE

/*
** Retry flock() calls that fail with EINTR
*/
#ifdef EINTR
static int robust_flock(int fd, int op){
  int rc;
  do{ rc = flock(fd,op); }while( rc<0 && errno==EINTR );
  return rc;
}
#else
# define robust_flock(a,b) flock(a,b)
#endif
   

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *res_out
** to a non-zero value otherwise *res_out is set to zero.  The return value
** is set to DBSQL_OK unless an I/O error occurs during lock checking.
*/
static int __flock_check_reserved_lock(dbsql_file_t *id, int *res_out){
#ifdef DBSQL_DEBUG
  unix_file *file = (unix_file*)id;
#else
  UNUSED_PARAMETER(id);
#endif

  SIMULATE_IO_ERROR( return DBSQL_IOERR_CHECKRESERVEDLOCK; );

  assert( file );
  assert( file->file_lock<=SHARED_LOCK );

  /* The flock VFS only ever takes exclusive locks (see function __flock_lock).
  ** Therefore, if this connection is holding any lock at all, no other
  ** connection may be holding a RESERVED lock. So set *res_out to 0
  ** in this case.
  **
  ** Or, this connection may be holding no lock. In that case, set *res_out to
  ** 0 as well. The caller will then attempt to take an EXCLUSIVE lock on the
  ** db in order to roll the hot journal back. If there is another connection
  ** holding a lock, that attempt will fail and an DBSQL_BUSY returned to
  ** the user. with_t other VFS, we try to avoid this, in order to allow a reader
  ** to proceed while a writer is preparing its transaction. But that won't
  ** work with the flock VFS - as it always takes EXCLUSIVE locks - so it is
  ** not a problem in this case.  */
  *res_out = 0;

  return DBSQL_OK;
}

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** flock() only really support EXCLUSIVE locks.  We track intermediate
** lock states in the dbsql_file_t structure, but all locks SHARED or
** above are really EXCLUSIVE locks and exclude all other processes from
** access the file.
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
*/
static int __flock_lock(dbsql_file_t *id, int file_lock) {
  int rc = DBSQL_OK;
  unix_file *file = (unix_file*)id;

  assert( file );

  /* if we already have a lock, it is exclusive.
  ** Just adjust level and punt on outta here. */
  if (file->file_lock > NO_LOCK) {
    file->file_lock = file_lock;
    return DBSQL_OK;
  }

  /* grab an exclusive lock */

  if (robust_flock(file->h, LOCK_EX | LOCK_NB)) {
    int t_errno = errno;
    /* didn't get, must be busy */
    rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_LOCK);
    if( IS_LOCK_ERROR(rc) ){
      __store_last_errno(file, t_errno);
    }
  } else {
    /* got it, set the type and return ok */
    file->file_lock = file_lock;
  }
  OSTRACE(("LOCK    %d %s %s (flock)\n", file->h, __az_file_lock(file_lock),
           rc==DBSQL_OK ? "ok" : "failed"));
#ifdef DBSQL_IGNORE_FLOCK_LOCK_ERRORS
  if( (rc & 0xff) == DBSQL_IOERR ){
    rc = DBSQL_BUSY;
  }
#endif /* DBSQL_IGNORE_FLOCK_LOCK_ERRORS */
  return rc;
}


/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int __flock_unlock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;

  assert( file );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (flock)\n", file->h, file_lock,
           file->file_lock, OS_GETPID(0)));
  assert( file_lock<=SHARED_LOCK );

  /* no-op if possible */
  if( file->file_lock==file_lock ){
    return DBSQL_OK;
  }

  /* shared can just be set because we always have an exclusive */
  if (file_lock==SHARED_LOCK) {
    file->file_lock = file_lock;
    return DBSQL_OK;
  }

  /* no, really, unlock. */
  if( robust_flock(file->h, LOCK_UN) ){
#ifdef DBSQL_IGNORE_FLOCK_LOCK_ERRORS
    return DBSQL_OK;
#endif /* DBSQL_IGNORE_FLOCK_LOCK_ERRORS */
    return DBSQL_IOERR_UNLOCK;
  }else{
    file->file_lock = NO_LOCK;
    return DBSQL_OK;
  }
}

/*
** Close a file.
*/
static int __flock_close(dbsql_file_t *id) {
  assert( id!=0 );
  __flock_unlock(id, NO_LOCK);
  return __close_unix_file(id);
}

#endif /* DBSQL_ENABLE_LOCKING_STYLE && !OS_VXWORK */

/******************* End of the flock lock implementation *********************
******************************************************************************/

/******************************************************************************
************************ Begin Named Semaphore Locking ************************
**
** Named semaphore locking is only supported on __vx_works.
**
** Semaphore locking is like dot-lock and flock in that it really only
** supports EXCLUSIVE locking.  Only a single process can read or write
** the database file at a time.  This reduces potential concurrency, but
** makes the lock implementation much easier.
*/
#if OS_VXWORKS

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *res_out
** to a non-zero value otherwise *res_out is set to zero.  The return value
** is set to DBSQL_OK unless an I/O error occurs during lock checking.
*/
static int __sem_x_check_reserved_lock(dbsql_file_t *id, int *res_out) {
  int rc = DBSQL_OK;
  int reserved = 0;
  unix_file *file = (unix_file*)id;

  SIMULATE_IO_ERROR( return DBSQL_IOERR_CHECKRESERVEDLOCK; );

  assert( file );

  /* Check if a thread in this process holds such a lock */
  if( file->file_lock>SHARED_LOCK ){
    reserved = 1;
  }

  /* Otherwise see if some other process holds it. */
  if( !reserved ){
    sem_t *sem = file->inode->sem;

    if( sem_trywait(sem)==-1 ){
      int t_errno = errno;
      if( EAGAIN != t_errno ){
        rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_CHECKRESERVEDLOCK);
        __store_last_errno(file, t_errno);
      } else {
        /* someone else has the lock when we are in NO_LOCK */
        reserved = (file->file_lock < SHARED_LOCK);
      }
    }else{
      /* we could have it if we want it */
      sem_post(sem);
    }
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (sem)\n", file->h, rc, reserved));

  *res_out = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** Semaphore locks only really support EXCLUSIVE locks.  We track intermediate
** lock states in the dbsql_file_t structure, but all locks SHARED or
** above are really EXCLUSIVE locks and exclude all other processes from
** access the file.
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
*/
static int __sem_x_lock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  sem_t *sem = file->inode->sem;
  int rc = DBSQL_OK;

  /* if we already have a lock, it is exclusive.
  ** Just adjust level and punt on outta here. */
  if (file->file_lock > NO_LOCK) {
    file->file_lock = file_lock;
    rc = DBSQL_OK;
    goto sem_end_lock;
  }

  /* lock semaphore now but bail out when already locked. */
  if( sem_trywait(sem)==-1 ){
    rc = DBSQL_BUSY;
    goto sem_end_lock;
  }

  /* got it, set the type and return ok */
  file->file_lock = file_lock;

 sem_end_lock:
  return rc;
}

/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int __sem_x_unlock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  sem_t *sem = file->inode->sem;

  assert( file );
  assert( sem );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (sem)\n", file->h, file_lock,
           file->file_lock, OS_GETPID(0)));
  assert( file_lock<=SHARED_LOCK );

  /* no-op if possible */
  if( file->file_lock==file_lock ){
    return DBSQL_OK;
  }

  /* shared can just be set because we always have an exclusive */
  if (file_lock==SHARED_LOCK) {
    file->file_lock = file_lock;
    return DBSQL_OK;
  }

  /* no, really unlock. */
  if ( sem_post(sem)==-1 ) {
    int rc, t_errno = errno;
    rc = dbsql_error_from_posix_error(t_errno, DBSQL_IOERR_UNLOCK);
    if( IS_LOCK_ERROR(rc) ){
      __store_last_errno(file, t_errno);
    }
    return rc;
  }
  file->file_lock = NO_LOCK;
  return DBSQL_OK;
}

/*
 ** Close a file.
 */
static int __sem_x_close(dbsql_file_t *id) {
  if( id ){
    unix_file *file = (unix_file*)id;
    __sem_x_unlock(id, NO_LOCK);
    assert( file );
    assert( __unix_file_mutex_notheld(file) );
    __unix_enter_mutex();
    __release_inode_info(file);
    __unix_leave_mutex();
    __close_unix_file(id);
  }
  return DBSQL_OK;
}

#endif /* OS_VXWORKS */
/*
** Named semaphore locking is only available on __vx_works.
**
*************** End of the named semaphore lock implementation ****************
******************************************************************************/


/******************************************************************************
*************************** Begin AFP Locking *********************************
**
** AFP is the Apple Filing Protocol.  AFP is a network filesystem found
** on Apple Macintosh computers - both OS9 and OSX.
**
** Third-party implementations of AFP are available.  But this code here
** only works on OSX.
*/

#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
/*
** The afp_locking_context structure contains all afp lock specific state
*/
typedef struct afp_locking_context afp_locking_context;
struct afp_locking_context {
  int reserved;
  const char *db_path;             /* Name of the open file */
};

struct byte_range_lock_pb2_t
{
  unsigned long long offset;        /* offset to first byte to lock */
  unsigned long long length;        /* nbr of bytes to lock */
  unsigned long long ret_range_start; /* nbr of 1st byte locked if successful */
  unsigned char un_lock_flag;         /* 1 = unlock, 0 = lock */
  unsigned char start_end_flag;       /* 1=rel to end of fork, 0=rel to start */
  int fd;                           /* file desc to assoc this lock with */
};

#define AFPFS_BYTE_RANGE_LOCK2_FSCTL        _IOWR('z', 23, struct byte_range_lock_pb2_t)

/*
** This is a utility for setting or clearing a bit-range lock on an
** AFP filesystem.
**
** Return DBSQL_OK on success, DBSQL_BUSY on failure.
*/
static int __afp_set_lock(
  const char *path,              /* Name of the file to be locked or unlocked */
  unix_file *file,               /* Open file descriptor on path */
  unsigned long long offset,     /* First byte to be locked */
  unsigned long long length,     /* Number of bytes to lock */
  int set_lock_flag                /* True to set lock.  False to clear lock */
){
  struct byte_range_lock_pb2_t pb;
  int err;

  pb.un_lock_flag = set_lock_flag ? 0 : 1;
  pb.start_end_flag = 0;
  pb.offset = offset;
  pb.length = length;
  pb.fd = file->h;

  OSTRACE(("AFPSETLOCK [%s] for %d%s in range %llx:%llx\n",
    (set_lock_flag?"ON":"OFF"), file->h, (pb.fd==-1?"[testval-1]":""),
    offset, length));
  err = fsctl(path, AFPFS_BYTE_RANGE_LOCK2_FSCTL, &pb, 0);
  if ( err==-1 ) {
    int rc;
    int t_errno = errno;
    OSTRACE(("AFPSETLOCK failed to fsctl() '%s' %d %s\n",
             path, t_errno, strerror(t_errno)));
#ifdef DBSQL_IGNORE_AFP_LOCK_ERRORS
    rc = DBSQL_BUSY;
#else
    rc = dbsql_error_from_posix_error(t_errno,
                    set_lock_flag ? DBSQL_IOERR_LOCK : DBSQL_IOERR_UNLOCK);
#endif /* DBSQL_IGNORE_AFP_LOCK_ERRORS */
    if( IS_LOCK_ERROR(rc) ){
      __store_last_errno(file, t_errno);
    }
    return rc;
  } else {
    return DBSQL_OK;
  }
}

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *res_out
** to a non-zero value otherwise *res_out is set to zero.  The return value
** is set to DBSQL_OK unless an I/O error occurs during lock checking.
*/
static int __afp_check_reserved_lock(dbsql_file_t *id, int *res_out){
  int rc = DBSQL_OK;
  int reserved = 0;
  unix_file *file = (unix_file*)id;
  afp_locking_context *context;

  SIMULATE_IO_ERROR( return DBSQL_IOERR_CHECKRESERVEDLOCK; );

  assert( file );
  context = (afp_locking_context *) file->locking_context;
  if( context->reserved ){
    *res_out = 1;
    return DBSQL_OK;
  }
  dbsql_mutex_enter(file->inode->lock_mutex);
  /* Check if a thread in this process holds such a lock */
  if( file->inode->file_lock>SHARED_LOCK ){
    reserved = 1;
  }

  /* Otherwise see if some other process holds it.
   */
  if( !reserved ){
    /* lock the RESERVED byte */
    int lrc = __afp_set_lock(context->db_path, file, RESERVED_BYTE, 1,1);
    if( DBSQL_OK==lrc ){
      /* if we succeeded in taking the reserved lock, unlock it to restore
      ** the original state */
      lrc = __afp_set_lock(context->db_path, file, RESERVED_BYTE, 1, 0);
    } else {
      /* if we failed to get the lock then someone else must have it */
      reserved = 1;
    }
    if( IS_LOCK_ERROR(lrc) ){
      rc=lrc;
    }
  }

  dbsql_mutex_leave(file->inode->lock_mutex);
  OSTRACE(("TEST WR-LOCK %d %d %d (afp)\n", file->h, rc, reserved));

  *res_out = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
*/
static int __afp_lock(dbsql_file_t *id, int file_lock){
  int rc = DBSQL_OK;
  unix_file *file = (unix_file*)id;
  unix_inode_info *inode = file->inode;
  afp_locking_context *context = (afp_locking_context *) file->locking_context;

  assert( file );
  OSTRACE(("LOCK    %d %s was %s(%s,%d) pid=%d (afp)\n", file->h,
           __az_file_lock(file_lock), __az_file_lock(file->file_lock),
           __az_file_lock(inode->file_lock), inode->n_shared , OS_GETPID(0)));

  /* If there is already a lock of this type or more restrictive on the
  ** unix_file, do nothing. Don't use the afp_end_lock: exit path, as
  ** __unix_enter_mutex() hasn't been called yet.
  */
  if( file->file_lock>=file_lock ){
    OSTRACE(("LOCK    %d %s ok (already held) (afp)\n", file->h,
           __az_file_lock(file_lock)));
    return DBSQL_OK;
  }

  /* Make sure the locking sequence is correct
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) dbsql never explicitly requests a pending lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  */
  assert( file->file_lock!=NO_LOCK || file_lock==SHARED_LOCK );
  assert( file_lock!=PENDING_LOCK );
  assert( file_lock!=RESERVED_LOCK || file->file_lock==SHARED_LOCK );

  /* This mutex is needed because file->inode is shared across threads
  */
  inode = file->inode;
  dbsql_mutex_enter(inode->lock_mutex);

  /* If some thread using this PID has a lock via a different unix_file*
  ** handle that precludes the requested lock, return BUSY.
  */
  if( (file->file_lock!=inode->file_lock &&
       (inode->file_lock>=PENDING_LOCK || file_lock>SHARED_LOCK))
     ){
    rc = DBSQL_BUSY;
    goto afp_end_lock;
  }

  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return DBSQL_OK.
  */
  if( file_lock==SHARED_LOCK &&
     (inode->file_lock==SHARED_LOCK || inode->file_lock==RESERVED_LOCK) ){
    assert( file_lock==SHARED_LOCK );
    assert( file->file_lock==0 );
    assert( inode->n_shared>0 );
    file->file_lock = SHARED_LOCK;
    inode->n_shared++;
    inode->n_lock++;
    goto afp_end_lock;
  }
  
  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  */
  if( file_lock==SHARED_LOCK
      || (file_lock==EXCLUSIVE_LOCK && file->file_lock<PENDING_LOCK)
  ){
    int failed;
    failed = __afp_set_lock(context->db_path, file, PENDING_BYTE, 1, 1);
    if (failed) {
      rc = failed;
      goto afp_end_lock;
    }
  }

  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  */
  if( file_lock==SHARED_LOCK ){
    int lrc1, lrc2, lrc1Errno = 0;
    long lk, mask;
  
    assert( inode->n_shared==0 );
    assert( inode->file_lock==0 );
      
    mask = (sizeof(long)==8) ? LARGEST_INT64 : 0x7fffffff;
    /* Now get the read-lock SHARED_LOCK */
    /* note that the quality of the randomness doesn't matter that much */
    lk = random();
    inode->shared_byte = (lk & mask)%(SHARED_SIZE - 1);
    lrc1 = __afp_set_lock(context->db_path, file,
          SHARED_FIRST+inode->shared_byte, 1, 1);
    if( IS_LOCK_ERROR(lrc1) ){
      lrc1Errno = file->last_errno;
    }
    /* Drop the temporary PENDING lock */
    lrc2 = __afp_set_lock(context->db_path, file, PENDING_BYTE, 1, 0);
  
    if( IS_LOCK_ERROR(lrc1) ) {
      __store_last_errno(file, lrc1Errno);
      rc = lrc1;
      goto afp_end_lock;
    } else if( IS_LOCK_ERROR(lrc2) ){
      rc = lrc2;
      goto afp_end_lock;
    } else if( lrc1 != DBSQL_OK ) {
      rc = lrc1;
    } else {
      file->file_lock = SHARED_LOCK;
      inode->n_lock++;
      inode->n_shared = 1;
    }
  }else if( file_lock==EXCLUSIVE_LOCK && inode->n_shared>1 ){
    /* We are trying for an exclusive lock but another thread in this
     ** same process is still holding a shared lock. */
    rc = DBSQL_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    */
    int failed = 0;
    assert( 0!=file->file_lock );
    if (file_lock >= RESERVED_LOCK && file->file_lock < RESERVED_LOCK) {
        /* Acquire a RESERVED lock */
        failed = __afp_set_lock(context->db_path, file, RESERVED_BYTE, 1,1);
      if( !failed ){
        context->reserved = 1;
      }
    }
    if (!failed && file_lock == EXCLUSIVE_LOCK) {
      /* Acquire an EXCLUSIVE lock */

      /* Remove the shared lock before trying the range.  we'll need to
      ** reestablish the shared lock if we can't get the  __afp_unlock
      */
      if( !(failed = __afp_set_lock(context->db_path, file, SHARED_FIRST +
                         inode->shared_byte, 1, 0)) ){
        int failed2 = DBSQL_OK;
        /* now attempt to get the exclusive lock range */
        failed = __afp_set_lock(context->db_path, file, SHARED_FIRST,
                               SHARED_SIZE, 1);
        if( failed && (failed2 = __afp_set_lock(context->db_path, file,
                       SHARED_FIRST + inode->shared_byte, 1, 1)) ){
          /* Can't reestablish the shared lock.  Sqlite can't deal, this is
          ** a critical I/O error
          */
          rc = ((failed & 0xff) == DBSQL_IOERR) ? failed2 :
               DBSQL_IOERR_LOCK;
          goto afp_end_lock;
        }
      }else{
        rc = failed;
      }
    }
    if( failed ){
      rc = failed;
    }
  }

  if( rc==DBSQL_OK ){
    file->file_lock = file_lock;
    inode->file_lock = file_lock;
  }else if( file_lock==EXCLUSIVE_LOCK ){
    file->file_lock = PENDING_LOCK;
    inode->file_lock = PENDING_LOCK;
  }

afp_end_lock:
  dbsql_mutex_leave(inode->lock_mutex);
  OSTRACE(("LOCK    %d %s %s (afp)\n", file->h, __az_file_lock(file_lock),
         rc==DBSQL_OK ? "ok" : "failed"));
  return rc;
}

/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int __afp_unlock(dbsql_file_t *id, int file_lock) {
  int rc = DBSQL_OK;
  unix_file *file = (unix_file*)id;
  unix_inode_info *inode;
  afp_locking_context *context = (afp_locking_context *) file->locking_context;
  int skip_shared = 0;

  assert( file );
  OSTRACE(("UNLOCK  %d %d was %d(%d,%d) pid=%d (afp)\n", file->h, file_lock,
           file->file_lock, file->inode->file_lock, file->inode->n_shared,
           OS_GETPID(0)));

  assert( file_lock<=SHARED_LOCK );
  if( file->file_lock<=file_lock ){
    return DBSQL_OK;
  }
  inode = file->inode;
  dbsql_mutex_enter(inode->lock_mutex);
  assert( inode->n_shared!=0 );
  if( file->file_lock>SHARED_LOCK ){
    assert( inode->file_lock==file->file_lock );
  
#ifdef DBSQL_DEBUG
    /* When reducing a lock such that other processes can start
    ** reading the database file again, make sure that the
    ** transaction counter was updated if any part of the database
    ** file changed.  If the transaction counter is not updated,
    ** other connections to the same file might not realize that
    ** the file has changed and hence might not know to flush their
    ** cache.  The use of a stale cache can lead to database corruption.
    */
    assert( file->in_normal_write==0
           || file->db_update==0
           || file->trans_cntr_chng==1 );
    file->in_normal_write = 0;
#endif
  
    if( file->file_lock==EXCLUSIVE_LOCK ){
      rc = __afp_set_lock(context->db_path, file, SHARED_FIRST, SHARED_SIZE, 0);
      if( rc==DBSQL_OK && (file_lock==SHARED_LOCK || inode->n_shared>1) ){
        /* only re-establish the shared lock if necessary */
        int shared_lock_byte = SHARED_FIRST+inode->shared_byte;
        rc = __afp_set_lock(context->db_path, file, shared_lock_byte, 1, 1);
      } else {
        skip_shared = 1;
      }
    }
    if( rc==DBSQL_OK && file->file_lock>=PENDING_LOCK ){
      rc = __afp_set_lock(context->db_path, file, PENDING_BYTE, 1, 0);
    }
    if( rc==DBSQL_OK && file->file_lock>=RESERVED_LOCK && context->reserved ){
      rc = __afp_set_lock(context->db_path, file, RESERVED_BYTE, 1, 0);
      if( !rc ){
        context->reserved = 0;
      }
    }
    if( rc==DBSQL_OK && (file_lock==SHARED_LOCK || inode->n_shared>1)){
      inode->file_lock = SHARED_LOCK;
    }
  }
  if( rc==DBSQL_OK && file_lock==NO_LOCK ){

    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    */
    unsigned long long shared_lock_byte = SHARED_FIRST+inode->shared_byte;
    inode->n_shared--;
    if( inode->n_shared==0 ){
      if( !skip_shared ){
        rc = __afp_set_lock(context->db_path, file, shared_lock_byte, 1, 0);
      }
      if( !rc ){
        inode->file_lock = NO_LOCK;
        file->file_lock = NO_LOCK;
      }
    }
    if( rc==DBSQL_OK ){
      inode->n_lock--;
      assert( inode->n_lock>=0 );
      if( inode->n_lock==0 ) __close_pending_fds(file);
    }
  }

  dbsql_mutex_leave(inode->lock_mutex);
  if( rc==DBSQL_OK ){
    file->file_lock = file_lock;
  }
  return rc;
}

/*
** Close a file & cleanup AFP specific locking context
*/
static int __afp_close(dbsql_file_t *id) {
  int rc = DBSQL_OK;
  unix_file *file = (unix_file*)id;
  assert( id!=0 );
  __afp_unlock(id, NO_LOCK);
  assert( __unix_file_mutex_notheld(file) );
  __unix_enter_mutex();
  if( file->inode ){
    unix_inode_info *inode = file->inode;
    dbsql_mutex_enter(inode->lock_mutex);
    if( inode->n_lock ){
      /* If there are outstanding locks, do not actually close the file just
      ** yet because that would clear those locks.  Instead, add the file
      ** descriptor to inode->pendings.  It will be automatically closed when
      ** the last lock is cleared.
      */
      __set_pending_fd(file);
    }
    dbsql_mutex_leave(inode->lock_mutex);
  }
  __release_inode_info(file);
  dbsql_free(file->locking_context);
  rc = __close_unix_file(id);
  __unix_leave_mutex();
  return rc;
}

#endif /* defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE */
/*
** The code above is the AFP lock implementation.  The code is specific
** to __mac_osx and does not work on other unix platforms.  No alternative
** is available.  If you don't compile for a mac, then the "unix-afp"
** VFS is not available.
**
********************* End of the AFP lock implementation **********************
******************************************************************************/

/******************************************************************************
*************************** Begin NFS Locking ********************************/

#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
/*
 ** Lower the locking level on file descriptor file to file_lock.  file_lock
 ** must be either NO_LOCK or SHARED_LOCK.
 **
 ** If the locking level of the file descriptor is already at or below
 ** the requested locking level, this routine is a no-op.
 */
static int __nfs_unlock(dbsql_file_t *id, int file_lock){
  return __posix_unlock(id, file_lock, 1);
}

#endif /* defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE */
/*
** The code above is the NFS lock implementation.  The code is specific
** to __mac_osx and does not work on other unix platforms.  No alternative
** is available.
**
********************* End of the NFS lock implementation **********************
******************************************************************************/

/******************************************************************************
**************** Non-locking dbsql_file_t methods *****************************
**
** The next division contains implementations for all methods of the
** dbsql_file_t object other than the locking methods.  The locking
** methods were defined in divisions above (one locking method per
** division).  Those methods that are common to all locking modes
** are gather together into this division.
*/

/*
** Seek to the offset passed as the second argument, then read cnt
** bytes into buf. Return the number of bytes actually read.
**
** To avoid stomping the errno value on a failed read the last_errno value
** is set before returning.
*/
static int __seek_and_read(unix_file *id, int64_t offset, void *buf, int cnt){
  int got;
  int prior = 0;
#if (!defined(USE_PREAD) && !defined(USE_PREAD64))
  int64_t newOffset;
#endif
  TIMER_START;
  assert( cnt==(cnt&0x1ffff) );
  assert( id->h>2 );
  do{
#if defined(USE_PREAD)
    got = OS_PREAD(id->h, buf, cnt, offset);
    SIMULATE_IO_ERROR( got = -1 );
#elif defined(USE_PREAD64)
    got = OS_PREAD64(id->h, buf, cnt, offset);
    SIMULATE_IO_ERROR( got = -1 );
#else
    newOffset = lseek(id->h, offset, SEEK_SET);
    SIMULATE_IO_ERROR( newOffset = -1 );
    if( newOffset<0 ){
      __store_last_errno((unix_file*)id, errno);
      return -1;
    }
    got = OS_READ(id->h, buf, cnt);
#endif
    if( got==cnt ) break;
    if( got<0 ){
      if( errno==EINTR ){ got = 1; continue; }
      prior = 0;
      __store_last_errno((unix_file*)id,  errno);
      break;
    }else if( got>0 ){
      cnt -= got;
      offset += got;
      prior += got;
      buf = (void*)(got + (char*)buf);
    }
  }while( got>0 );
  TIMER_END;
  OSTRACE(("READ    %-3d %5d %7lld %llu\n",
            id->h, got+prior, offset-prior, TIMER_ELAPSED));
  return got+prior;
}

/*
** Read data from a file into a buffer.  Return DBSQL_OK if all
** bytes were read successfully and DBSQL_IOERR if anything goes
** wrong.
*/
static int __unix_read(
  dbsql_file_t *id,
  void *buf,
  int amt,
  int64_t offset
){
  unix_file *file = (unix_file *)id;
  int got;
  assert( id );
  assert( offset>=0 );
  assert( amt>0 );

  /* If this is a database file (not a journal, super-journal or temp
  ** file), the bytes in the locking range should never be read or written. */
#if 0
  assert( file->preallocated_unused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE
  );
#endif

#if DBSQL_MAX_MMAP_SIZE>0
  /* Deal with as much of this read request as possible by transferring
  ** data from the memory mapping using memcpy().  */
  if( offset<file->mmap_size ){
    if( offset+amt <= file->mmap_size ){
      memcpy(buf, &((u_int8_t *)(file->map_region))[offset], amt);
      return DBSQL_OK;
    }else{
      int n_copy = file->mmap_size - offset;
      memcpy(buf, &((u_int8_t *)(file->map_region))[offset], n_copy);
      buf = &((u_int8_t *)buf)[n_copy];
      amt -= n_copy;
      offset += n_copy;
    }
  }
#endif

  got = __seek_and_read(file, offset, buf, amt);
  if( got==amt ){
    return DBSQL_OK;
  }else if( got<0 ){
    /* file->last_errno has been set by __seek_and_read().
    ** Usually we return DBSQL_IOERR_READ here, though for some
    ** kinds of errors we return DBSQL_IOERR_CORRUPTFS.  The
    ** DBSQL_IOERR_CORRUPTFS will be converted into DBSQL_CORRUPT
    ** prior to returning to the application by the __api_exit()
    ** routine.
    */
    switch( file->last_errno ){
      case ERANGE:
      case EIO:
#ifdef ENXIO
      case ENXIO:
#endif
#ifdef EDEVERR
      case EDEVERR:
#endif
        return DBSQL_IOERR_CORRUPTFS;
    }
    return DBSQL_IOERR_READ;
  }else{
    __store_last_errno(file, 0);   /* not a system error */
    /* Unread parts of the buffer must be zero-filled */
    memset(&((char*)buf)[got], 0, amt-got);
    return DBSQL_IOERR_SHORT_READ;
  }
}

/*
** Attempt to seek the file-descriptor passed as the first argument to
** absolute offset i_off, then attempt to write n_buf bytes of data from
** buf to it. If an error occurs, return -1 and set *pi_errno. Otherwise,
** return the actual number of bytes written (which may be less than
** n_buf).
*/
static int __seek_and_write_fd(
  int fd,                         /* File descriptor to write to */
  int64_t i_off,                       /* File offset to begin writing at */
  const void *buf,               /* Copy data from this buffer to the file */
  int n_buf,                       /* Size of buffer buf in bytes */
  int *pi_errno                    /* OUT: Error number if error occurs */
){
  int rc = 0;                     /* Value returned by system call */

  assert( n_buf==(n_buf&0x1ffff) );
  assert( fd>2 );
  assert( pi_errno!=0 );
  n_buf &= 0x1ffff;
  TIMER_START;

#if defined(USE_PREAD)
  do{ rc = (int)OS_PWRITE(fd, buf, n_buf, i_off); }while( rc<0 && errno==EINTR );
#elif defined(USE_PREAD64)
  do{ rc = (int)OS_PWRITE64(fd, buf, n_buf, i_off);}while( rc<0 && errno==EINTR);
#else
  do{
    int64_t i_seek = lseek(fd, i_off, SEEK_SET);
    SIMULATE_IO_ERROR( i_seek = -1 );
    if( i_seek<0 ){
      rc = -1;
      break;
    }
    rc = OS_WRITE(fd, buf, n_buf);
  }while( rc<0 && errno==EINTR );
#endif

  TIMER_END;
  OSTRACE(("WRITE   %-3d %5d %7lld %llu\n", fd, rc, i_off, TIMER_ELAPSED));

  if( rc<0 ) *pi_errno = errno;
  return rc;
}


/*
** Seek to the offset in id->offset then read cnt bytes into buf.
** Return the number of bytes actually read.  Update the offset.
**
** To avoid stomping the errno value on a failed write the last_errno value
** is set before returning.
*/
static int __seek_and_write(unix_file *id, int64_t offset, const void *buf, int cnt){
  return __seek_and_write_fd(id->h, offset, buf, cnt, &id->last_errno);
}


/*
** Write data from a buffer into a file.  Return DBSQL_OK on success
** or some other error code on failure.
*/
static int __unix_write(
  dbsql_file_t *id,
  const void *buf,
  int amt,
  int64_t offset
){
  unix_file *file = (unix_file*)id;
  int wrote = 0;
  assert( id );
  assert( amt>0 );

  /* If this is a database file (not a journal, super-journal or temp
  ** file), the bytes in the locking range should never be read or written. */
#if 0
  assert( file->preallocated_unused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE
  );
#endif

#ifdef DBSQL_DEBUG
  /* If we are doing a normal write to a database file (as opposed to
  ** doing a hot-journal rollback or a write to some file other than a
  ** normal database file) then record the fact that the database
  ** has changed.  If the transaction counter is modified, record that
  ** fact too.
  */
  if( file->in_normal_write ){
    file->db_update = 1;  /* The database has been modified */
    if( offset<=24 && offset+amt>=27 ){
      int rc;
      char old_cntr[4];
      SIMULATE_IO_ERROR_BENIGN(1);
      rc = __seek_and_read(file, 24, old_cntr, 4);
      SIMULATE_IO_ERROR_BENIGN(0);
      if( rc!=4 || memcmp(old_cntr, &((char*)buf)[24-offset], 4)!=0 ){
        file->trans_cntr_chng = 1;  /* The transaction counter has changed */
      }
    }
  }
#endif

#if defined(DBSQL_MMAP_READWRITE) && DBSQL_MAX_MMAP_SIZE>0
  /* Deal with as much of this write request as possible by transferring
  ** data from the memory mapping using memcpy().  */
  if( offset<file->mmap_size ){
    if( offset+amt <= file->mmap_size ){
      memcpy(&((u_int8_t *)(file->map_region))[offset], buf, amt);
      return DBSQL_OK;
    }else{
      int n_copy = file->mmap_size - offset;
      memcpy(&((u_int8_t *)(file->map_region))[offset], buf, n_copy);
      buf = &((u_int8_t *)buf)[n_copy];
      amt -= n_copy;
      offset += n_copy;
    }
  }
#endif

  while( (wrote = __seek_and_write(file, offset, buf, amt))<amt && wrote>0 ){
    amt -= wrote;
    offset += wrote;
    buf = &((char*)buf)[wrote];
  }
  SIMULATE_IO_ERROR(( wrote=(-1), amt=1 ));
  SIMULATE_DISKFULL_ERROR(( wrote=0, amt=1 ));

  if( amt>wrote ){
    if( wrote<0 && file->last_errno!=ENOSPC ){
      /* last_errno set by __seek_and_write */
      return DBSQL_IOERR_WRITE;
    }else{
      __store_last_errno(file, 0); /* not a system error */
      return DBSQL_FULL;
    }
  }

  return DBSQL_OK;
}

#ifdef DBSQL_TEST
/*
** Count the number of fullsyncs and normal syncs.  This is used to test
** that syncs and fullsyncs are occurring at the right times.
*/
int dbsql_sync_count = 0;
int dbsql_fullsync_count = 0;
#endif

/*
** We do not trust systems to provide a working fdatasync().  Some do.
** Others do no.  To be safe, we will stick with the (slightly slower)
** fsync(). If you know that your system does support fdatasync() correctly,
** then simply compile with -Dfdatasync=fdatasync or -DHAVE_FDATASYNC
*/
#if !defined(fdatasync) && !HAVE_FDATASYNC
# define fdatasync fsync
#endif

/*
** Define HAVE_FULLFSYNC to 0 or 1 depending on whether or not
** the F_FULLFSYNC macro is defined.  F_FULLFSYNC is currently
** only available on Mac OS X.  But that could change.
*/
#ifdef F_FULLFSYNC
# define HAVE_FULLFSYNC 1
#else
# define HAVE_FULLFSYNC 0
#endif


/*
** The fsync() system call does not work as advertised on many
** unix systems.  The following procedure is an attempt to make
** it work better.
**
** The DBSQL_NO_SYNC macro disables all fsync()s.  This is useful
** for testing when we want to run through the test suite quickly.
** You are strongly advised *not* to deploy with DBSQL_NO_SYNC
** enabled, however, since with DBSQL_NO_SYNC enabled, an OS crash
** or power failure will likely corrupt the database file.
**
** dbsql sets the data_only flag if the size of the file is unchanged.
** The idea behind data_only is that it should only write the file content
** to disk, not the inode.  We only set data_only if the file size is
** unchanged since the file size is part of the inode.  However,
** Ted Ts'o tells us that fdatasync() will also write the inode if the
** file size has changed.  The only real difference between fdatasync()
** and fsync(), Ted tells us, is that fdatasync() will not flush the
** inode if the mtime or owner or other inode attributes have changed.
** We only care about the file size, not the other file attributes, so
** as far as dbsql is concerned, an fdatasync() is always adequate.
** So, we always use fdatasync() if it is available, regardless of
** the value of the data_only flag.
*/
static int full_fsync(int fd, int full_sync, int data_only){
  int rc;

  /* The following "ifdef/elif/else/" block has the same structure as
  ** the one below. It is replicated here solely to avoid cluttering
  ** up the real code with the UNUSED_PARAMETER() macros.
  */
#ifdef DBSQL_NO_SYNC
  UNUSED_PARAMETER(fd);
  UNUSED_PARAMETER(full_sync);
  UNUSED_PARAMETER(data_only);
#elif HAVE_FULLFSYNC
  UNUSED_PARAMETER(data_only);
#else
  UNUSED_PARAMETER(full_sync);
  UNUSED_PARAMETER(data_only);
#endif

  /* Record the number of times that we do a normal fsync() and
  ** FULLSYNC.  This is used during testing to verify that this procedure
  ** gets called with the correct arguments.
  */
#ifdef DBSQL_TEST
  if( full_sync ) dbsql_fullsync_count++;
  dbsql_sync_count++;
#endif

  /* If we compiled with the DBSQL_NO_SYNC flag, then syncing is a
  ** no-op.  But go ahead and call fstat() to validate the file
  ** descriptor as we need a method to provoke a failure during
  ** coverage testing.
  */
#ifdef DBSQL_NO_SYNC
  {
    struct stat buf;
    rc = OS_FSTAT(fd, &buf);
  }
#elif HAVE_FULLFSYNC
  if( full_sync ){
    rc = __os_fcntl(fd, F_FULLFSYNC, 0);
  }else{
    rc = 1;
  }
  /* If the FULLFSYNC failed, fall back to attempting an fsync().
  ** It shouldn't be possible for fullfsync to fail on the local
  ** file system (on OSX), so failure indicates that FULLFSYNC
  ** isn't supported for this file system. So, attempt an fsync
  ** and (for now) ignore the overhead of a superfluous fcntl call.
  ** It'd be better to detect fullfsync support once and avoid
  ** the fcntl call every time sync is called.
  */
  if( rc ) rc = fsync(fd);

#elif defined(__APPLE__)
  /* fdatasync() on HFS+ doesn't yet flush the file size if it changed correctly
  ** so currently we default to the macro that redefines fdatasync to fsync
  */
  rc = fsync(fd);
#else
  rc = fdatasync(fd);
#if OS_VXWORKS
  if( rc==-1 && errno==ENOTSUP ){
    rc = fsync(fd);
  }
#endif /* OS_VXWORKS */
#endif /* ifdef DBSQL_NO_SYNC elif HAVE_FULLFSYNC */

  if( OS_VXWORKS && rc!= -1 ){
    rc = 0;
  }
  return rc;
}

/*
** Open a file descriptor to the directory containing file filename.
** If successful, *fd is set to the opened file descriptor and
** DBSQL_OK is returned. If an error occurs, either DBSQL_NOMEM
** or DBSQL_CANTOPEN is returned and *fd is set to an undefined
** value.
**
** The directory file descriptor is used for only one thing - to
** fsync() a directory to make sure file creation and deletion events
** are flushed to disk.  Such fsyncs are not needed on newer
** journaling filesystems, but are required on older filesystems.
**
** This routine can be overridden using the set_sys_call interface.
** The ability to override this routine was added in support of the
** chromium sandbox.  Opening a directory is a security risk (we are
** told) so making it overrideable allows the chromium sandbox to
** replace this routine with a harmless no-op.  To make this routine
** a no-op, replace it with a stub that returns DBSQL_OK but leaves
** *fd set to a negative number.
**
** If DBSQL_OK is returned, the caller is responsible for closing
** the file descriptor *fd using close().
*/
static int __open_directory(const char *filename, int *p_fd){
  int ii;
  int fd = -1;
  char dirname[MAX_PATHNAME+1];

  dbsql_snprintf(MAX_PATHNAME, dirname, "%s", filename);
  for(ii=(int)strlen(dirname); ii>0 && dirname[ii]!='/'; ii--);
  if( ii>0 ){
    dirname[ii] = '\0';
  }else{
    if( dirname[0]!='/' ) dirname[0] = '.';
    dirname[1] = 0;
  }
  fd = robust_open(dirname, O_RDONLY|O_BINARY, 0);
  if( fd>=0 ){
    OSTRACE(("OPENDIR %-3d %s\n", fd, dirname));
  }
  *p_fd = fd;
  if( fd>=0 ) return DBSQL_OK;
  return UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "openDirectory", dirname);
}

/*
** Make sure all writes to a particular file are committed to disk.
**
** If data_only==0 then both the file itself and its metadata (file
** size, access time, etc) are synced.  If data_only!=0 then only the
** file data is synced.
**
** Under Unix, also make sure that the directory entry for the file
** has been created by fsync-ing the directory that contains the file.
** If we do not do this and we encounter a power failure, the directory
** entry for the journal might not exist after we reboot.  The next
** dbsql to access the file will not know that the journal exists (because
** the directory entry for the journal was never created) and the transaction
** will not roll back - possibly leading to database corruption.
*/
static int __unix_sync(dbsql_file_t *id, int flags){
  int rc;
  unix_file *file = (unix_file*)id;

  int is_data_only = (flags&DBSQL_SYNC_DATAONLY);
  int is_fullsync = (flags&0x0F)==DBSQL_SYNC_FULL;

  /* Check that one of DBSQL_SYNC_NORMAL or FULL was passed */
  assert((flags&0x0F)==DBSQL_SYNC_NORMAL
      || (flags&0x0F)==DBSQL_SYNC_FULL
  );

  /* Unix cannot, but some systems may return DBSQL_FULL from here. This
  ** line is to test that doing so does not cause any problems.
  */
  SIMULATE_DISKFULL_ERROR( return DBSQL_FULL );

  assert( file );
  OSTRACE(("SYNC    %-3d\n", file->h));
  rc = full_fsync(file->h, is_fullsync, is_data_only);
  SIMULATE_IO_ERROR( rc=1 );
  if( rc ){
    __store_last_errno(file, errno);
    return UNIX_LOG_ERROR(DBSQL_IOERR_FSYNC, "full_fsync", file->path);
  }

  /* Also fsync the directory containing the file if the DIRSYNC flag
  ** is set.  This is a one-time occurrence.  Many systems (examples: AIX)
  ** are unable to fsync a directory, so ignore errors on the fsync.
  */
  if( file->ctrl_flags & UNIXFILE_DIRSYNC ){
    int dirfd;
    OSTRACE(("DIRSYNC %s (have_fullfsync=%d fullsync=%d)\n", file->path,
            HAVE_FULLFSYNC, is_fullsync));
    rc = OS_OPEN_DIRECTORY(file->path, &dirfd);
    if( rc==DBSQL_OK ){
      full_fsync(dirfd, 0, 0);
      robust_close(file, dirfd, __LINE__);
    }else{
      assert( rc==DBSQL_CANTOPEN );
      rc = DBSQL_OK;
    }
    file->ctrl_flags &= ~UNIXFILE_DIRSYNC;
  }
  return rc;
}

/*
** Truncate an open file to a specified size
*/
static int __unix_truncate(dbsql_file_t *id, int64_t n_byte){
  unix_file *file = (unix_file *)id;
  int rc;
  assert( file );
  SIMULATE_IO_ERROR( return DBSQL_IOERR_TRUNCATE );

  /* If the user has configured a chunk-size for this file, truncate the
  ** file so that it consists of an integer number of chunks (i.e. the
  ** actual file size after the operation may be larger than the requested
  ** size).
  */
  if( file->sz_chunk>0 ){
    n_byte = ((n_byte + file->sz_chunk - 1)/file->sz_chunk) * file->sz_chunk;
  }

  rc = robust_ftruncate(file->h, n_byte);
  if( rc ){
    __store_last_errno(file, errno);
    return UNIX_LOG_ERROR(DBSQL_IOERR_TRUNCATE, "ftruncate", file->path);
  }else{
#ifdef DBSQL_DEBUG
    /* If we are doing a normal write to a database file (as opposed to
    ** doing a hot-journal rollback or a write to some file other than a
    ** normal database file) and we truncate the file to zero length,
    ** that effectively updates the change counter.  This might happen
    ** when restoring a database using the backup API from a zero-length
    ** source.
    */
    if( file->in_normal_write && n_byte==0 ){
      file->trans_cntr_chng = 1;
    }
#endif

#if DBSQL_MAX_MMAP_SIZE>0
    /* If the file was just truncated to a size smaller than the currently
    ** mapped region, reduce the effective mapping size as well. dbsql will
    ** use read() and write() to access data beyond this point from now on.
    */
    if( n_byte<file->mmap_size ){
      file->mmap_size = n_byte;
    }
#endif

    return DBSQL_OK;
  }
}

/*
** Determine the current size of a file in bytes
*/
static int __unix_file_size(dbsql_file_t *id, int64_t *size){
  int rc;
  struct stat buf;
  assert( id );
  rc = OS_FSTAT(((unix_file*)id)->h, &buf);
  SIMULATE_IO_ERROR( rc=1 );
  if( rc!=0 ){
    __store_last_errno((unix_file*)id, errno);
    return DBSQL_IOERR_FSTAT;
  }
  *size = buf.st_size;

  /* When opening a zero-size database, the __find_inode_info() procedure
  ** writes a single byte into that file in order to work around a bug
  ** in the OS-X msdos filesystem.  In order to avoid problems with upper
  ** layers, we need to report this file size as zero even though it is
  ** really 1.   Ticket #3260.
  */
  if( *size==1 ) *size = 0;


  return DBSQL_OK;
}

#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
/*
** Handler for proxy-locking file-control verbs.  Defined below in the
** proxying locking division.
*/
static int __proxy_file_control(dbsql_file_t*,int,void*);
#endif

/*
** This function is called to handle the DBSQL_FCNTL_SIZE_HINT
** file-control operation.  Enlarge the database to n_bytes in size
** (rounded up to the next chunk-size).  If the database is already
** n_bytes or larger, this routine is a no-op.
*/
static int __fcntl_size_hint(unix_file *file, int64_t n_byte){
  if( file->sz_chunk>0 ){
    int64_t n_size;                    /* Required file size */
    struct stat buf;              /* Used to hold return values of fstat() */
 
    if( OS_FSTAT(file->h, &buf) ){
      return DBSQL_IOERR_FSTAT;
    }

    n_size = ((n_byte+file->sz_chunk-1) / file->sz_chunk) * file->sz_chunk;
    if( n_size>(int64_t)buf.st_size ){

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
      /* The code below is handling the return value of OS_FALLOCATE()
      ** correctly. posix_fallocate() is defined to "returns zero on success,
      ** or an error number on  failure". See the manpage for details. */
      int err;
      do{
        err = OS_FALLOCATE(file->h, buf.st_size, n_size-buf.st_size);
      }while( err==EINTR );
      if( err && err!=EINVAL ) return DBSQL_IOERR_WRITE;
#else
      /* If the OS does not have posix_fallocate(), fake it. Write a
      ** single byte to the last byte in each block that falls entirely
      ** within the extended region. Then, if required, a single byte
      ** at offset (n_size-1), to set the size of the file correctly.
      ** This is a similar technique to that used by glibc on systems
      ** that do not have a real fallocate() call.
      */
      int n_blk = buf.st_blksize;  /* File-system block size */
      int n_write = 0;             /* Number of bytes written by __seek_and_write */
      int64_t i_write;                 /* Next offset to write to */

      i_write = (buf.st_size/n_blk)*n_blk + n_blk - 1;
      assert( i_write>=buf.st_size );
      assert( ((i_write+1)%n_blk)==0 );
      for(/*no-op*/; i_write<n_size+n_blk-1; i_write+=n_blk ){
        if( i_write>=n_size ) i_write = n_size - 1;
        n_write = __seek_and_write(file, i_write, "", 1);
        if( n_write!=1 ) return DBSQL_IOERR_WRITE;
      }
#endif
    }
  }

#if DBSQL_MAX_MMAP_SIZE>0
  if( file->mmap_size_max>0 && n_byte>file->mmap_size ){
    int rc;
    if( file->sz_chunk<=0 ){
      if( robust_ftruncate(file->h, n_byte) ){
        __store_last_errno(file, errno);
        return UNIX_LOG_ERROR(DBSQL_IOERR_TRUNCATE, "ftruncate", file->path);
      }
    }

    rc = __unix_mapfile(file, n_byte);
    return rc;
  }
#endif

  return DBSQL_OK;
}

/*
** If *arg is initially negative then this is a query.  Set *arg to
** 1 or 0 depending on whether or not bit mask of file->ctrl_flags is set.
**
** If *arg is 0 or 1, then clear or set the mask bit of file->ctrl_flags.
*/
static void __unix_mode_bit(unix_file *file, unsigned char mask, int *arg){
  if( *arg<0 ){
    *arg = (file->ctrl_flags & mask)!=0;
  }else if( (*arg)==0 ){
    file->ctrl_flags &= ~mask;
  }else{
    file->ctrl_flags |= mask;
  }
}

/* Forward declaration */
static int __unix_get_tempname(int n_buf, char *buf);
#if !defined(DBSQL_WASI) && !defined(DBSQL_OMIT_WAL)
 static int __unix_fcntl_external_reader(unix_file*, int*);
#endif
#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
 static void __unix_describe_shm(dbsql_str_t*,unix_shm*);
#endif


/*
** Information and control of an open file handle.
*/
static int __unix_file_control(dbsql_file_t *id, int op, void *arg){
  unix_file *file = (unix_file*)id;
  switch( op ){
#if defined(__linux__) && defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
    case DBSQL_FCNTL_BEGIN_ATOMIC_WRITE: {
      int rc = OS_IOCTL(file->h, F2FS_IOC_START_ATOMIC_WRITE);
      return rc ? DBSQL_IOERR_BEGIN_ATOMIC : DBSQL_OK;
    }
    case DBSQL_FCNTL_COMMIT_ATOMIC_WRITE: {
      int rc = OS_IOCTL(file->h, F2FS_IOC_COMMIT_ATOMIC_WRITE);
      return rc ? DBSQL_IOERR_COMMIT_ATOMIC : DBSQL_OK;
    }
    case DBSQL_FCNTL_ROLLBACK_ATOMIC_WRITE: {
      int rc = OS_IOCTL(file->h, F2FS_IOC_ABORT_VOLATILE_WRITE);
      return rc ? DBSQL_IOERR_ROLLBACK_ATOMIC : DBSQL_OK;
    }
#endif /* __linux__ && DBSQL_ENABLE_BATCH_ATOMIC_WRITE */

    case DBSQL_FCNTL_NULL_IO: {
      OS_CLOSE(file->h);
      file->h = -1;
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_LOCKSTATE: {
      *(int*)arg = file->file_lock;
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_LAST_ERRNO: {
      *(int*)arg = file->last_errno;
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_CHUNK_SIZE: {
      file->sz_chunk = *(int *)arg;
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_SIZE_HINT: {
      int rc;
      SIMULATE_IO_ERROR_BENIGN(1);
      rc = __fcntl_size_hint(file, *(int64_t *)arg);
      SIMULATE_IO_ERROR_BENIGN(0);
      return rc;
    }
    case DBSQL_FCNTL_PERSIST_WAL: {
      __unix_mode_bit(file, UNIXFILE_PERSIST_WAL, (int*)arg);
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_POWERSAFE_OVERWRITE: {
      __unix_mode_bit(file, UNIXFILE_PSOW, (int*)arg);
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_VFSNAME: {
      *(char**)arg = dbsql_mprintf("%s", file->vfs->name);
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_TEMPFILENAME: {
      char *t_file = dbsql_malloc64( file->vfs->mx_pathname );
      if( t_file ){
        __unix_get_tempname(file->vfs->mx_pathname, t_file);
        *(char**)arg = t_file;
      }
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_HAS_MOVED: {
      *(int*)arg = __file_has_moved(file);
      return DBSQL_OK;
    }
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    case DBSQL_FCNTL_LOCK_TIMEOUT: {
      int i_old = file->i_busy_timeout;
      int i_new = *(int*)arg;
#if DBSQL_ENABLE_SETLK_TIMEOUT==1
      file->i_busy_timeout = i_new<0 ? 0x7FFFFFFF : (unsigned)i_new;
#elif DBSQL_ENABLE_SETLK_TIMEOUT==2
      file->i_busy_timeout = !!(*(int*)arg);
#else
# error "DBSQL_ENABLE_SETLK_TIMEOUT must be set to 1 or 2"
#endif
      *(int*)arg = i_old;
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_BLOCK_ON_CONNECT: {
      int i_new = *(int*)arg;
      file->block_on_connect = i_new;
      return DBSQL_OK;
    }
#endif /* DBSQL_ENABLE_SETLK_TIMEOUT */
#if DBSQL_MAX_MMAP_SIZE>0
    case DBSQL_FCNTL_MMAP_SIZE: {
      int64_t new_limit = *(int64_t*)arg;
      int rc = DBSQL_OK;
      if( new_limit>__global_config.mx_mmap ){
        new_limit = __global_config.mx_mmap;
      }

      /* The value of new_limit may be eventually cast to (size_t) and passed
      ** to mmap(). Restrict its value to 2GB if (size_t) is not at least a
      ** 64-bit type. */
      if( new_limit>0 && sizeof(size_t)<8 ){
        new_limit = (new_limit & 0x7FFFFFFF);
      }

      *(int64_t*)arg = file->mmap_size_max;
      if( new_limit>=0 && new_limit!=file->mmap_size_max && file->n_fetch_out==0 ){
        file->mmap_size_max = new_limit;
        if( file->mmap_size>0 ){
          __unix_unmapfile(file);
          rc = __unix_mapfile(file, -1);
        }
      }
      return rc;
    }
#endif
#ifdef DBSQL_DEBUG
    /* The pager calls this method to signal that it has done
    ** a rollback and that the database is therefore unchanged and
    ** it hence it is OK for the transaction change counter to be
    ** unchanged.
    */
    case DBSQL_FCNTL_DB_UNCHANGED: {
      ((unix_file*)id)->db_update = 0;
      return DBSQL_OK;
    }
#endif
#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
    case DBSQL_FCNTL_SET_LOCKPROXYFILE:
    case DBSQL_FCNTL_GET_LOCKPROXYFILE: {
      return __proxy_file_control(id,op,arg);
    }
#endif /* DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__) */

    case DBSQL_FCNTL_EXTERNAL_READER: {
#if !defined(DBSQL_WASI) && !defined(DBSQL_OMIT_WAL)
      return __unix_fcntl_external_reader((unix_file*)id, (int*)arg);
#else
      *(int*)arg = 0;
      return DBSQL_OK;
#endif
    }

#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
    case DBSQL_FCNTL_FILESTAT: {
      dbsql_str_t *str = (dbsql_str_t*)arg;
      char lcks[16];
      unix_inode_info *inode;
      static const char *locks[] = { "SHARED", "RESERVED",
                                      "PENDING", "EXCLUSIVE" };
      dbsql_str_appendf(str, "{\"h\":%d", file->h);
      dbsql_str_appendf(str, ",\"vfs\":\"%s\"", file->vfs->name);
      if( file->file_lock ){
        dbsql_str_appendf(str, ",\"eFileLock\":\"%s\"", 
                                  locks[file->file_lock-1]);
        if( UNIX_POSIX_ADVISORY_LOCKS(file->h, lcks)==DBSQL_OK ){
          dbsql_str_appendf(str, ",\"pal\":\"%s\"", lcks);
        }
      }
      __unix_enter_mutex();
#ifndef DBSQL_OMIT_WAL
      if( file->shm ){
        dbsql_str_appendall(str, ",\"shm\":");
        __unix_describe_shm(str, file->shm);
      }
#endif
#if DBSQL_MAX_MMAP_SIZE>0
      if( file->mmap_size ){
        dbsql_str_appendf(str, ",\"mmapSize\":%lld", file->mmap_size);
        dbsql_str_appendf(str, ",\"nFetchOut\":%d", file->n_fetch_out);
      }
#endif
      if( (inode = file->inode)!=0 ){
        dbsql_str_appendf(str, ",\"inode\":{\"nRef\":%d",inode->n_ref);
        dbsql_mutex_enter(inode->lock_mutex);
        dbsql_str_appendf(str, ",\"nShared\":%d", inode->n_shared);
        if( inode->file_lock ){
          dbsql_str_appendf(str, ",\"eFileLock\":\"%s\"", 
                                  locks[inode->file_lock-1]);
        }
        if( inode->unused ){
          char c_sep = '[';
          unix_unused_fd_t *u_fd = file->inode->unused;
          dbsql_str_appendall(str, ",\"unusedFd\":");
          while( u_fd ){
            dbsql_str_appendf(str, "%c{\"fd\":%d,\"flags\":%d",
                                c_sep, u_fd->fd, u_fd->flags);
            c_sep = ',';
            if( UNIX_POSIX_ADVISORY_LOCKS(u_fd->fd, lcks)==DBSQL_OK ){
              dbsql_str_appendf(str, ",\"pal\":\"%s\"", lcks);
            }
            dbsql_str_append(str, "}", 1);
            u_fd = u_fd->next;
          }
          dbsql_str_append(str, "]", 1);
        }
        dbsql_mutex_leave(inode->lock_mutex);
        dbsql_str_append(str, "}", 1);
      }
      __unix_leave_mutex();
      dbsql_str_append(str, "}", 1);
      return DBSQL_OK;
    }
#endif /* DBSQL_DEBUG || DBSQL_ENABLE_FILESTAT */
  }
  return DBSQL_NOTFOUND;
}

/*
** If fd->sector_size is non-zero when this function is called, it is a
** no-op. Otherwise, the values of fd->sector_size and
** fd->device_characteristics are set according to the file-system
** characteristics.
**
** There are two versions of this function. One for QNX and one for all
** other systems.
*/
#ifndef __QNXNTO__
static void __set_device_characteristics(unix_file *fd){
  assert( fd->device_characteristics==0 || fd->sector_size!=0 );
  if( fd->sector_size==0 ){
#if defined(__linux__) && defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
    int res;
    u_int32_t f = 0;

    /* Check for support for F2FS atomic batch writes. */
    res = OS_IOCTL(fd->h, F2FS_IOC_GET_FEATURES, &f);
    if( res==0 && (f & F2FS_FEATURE_ATOMIC_WRITE) ){
      fd->device_characteristics = DBSQL_IOCAP_BATCH_ATOMIC;
    }
#endif /* __linux__ && DBSQL_ENABLE_BATCH_ATOMIC_WRITE */

    /* Set the POWERSAFE_OVERWRITE flag if requested. */
    if( fd->ctrl_flags & UNIXFILE_PSOW ){
      fd->device_characteristics |= DBSQL_IOCAP_POWERSAFE_OVERWRITE;
    }
    fd->device_characteristics |= DBSQL_IOCAP_SUBPAGE_READ;

    fd->sector_size = DBSQL_DEFAULT_SECTOR_SIZE;
  }
}
#else
#include <sys/dcmd_blk.h>
#include <sys/statvfs.h>
static void __set_device_characteristics(unix_file *file){
  if( file->sector_size == 0 ){
    struct statvfs fs_info;

    /* Set defaults for non-supported filesystems */
    file->sector_size = DBSQL_DEFAULT_SECTOR_SIZE;
    file->device_characteristics = 0;
    if( fstatvfs(file->h, &fs_info) == -1 ) {
      return;
    }

    if( !strcmp(fs_info.f_basetype, "tmp") ) {
      file->sector_size = fs_info.f_bsize;
      file->device_characteristics =
        DBSQL_IOCAP_ATOMIC4K |       /* All ram filesystem writes are atomic */
        DBSQL_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        DBSQL_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( strstr(fs_info.f_basetype, "etfs") ){
      file->sector_size = fs_info.f_bsize;
      file->device_characteristics =
        /* etfs cluster size writes are atomic */
        (file->sector_size / 512 * DBSQL_IOCAP_ATOMIC512) |
        DBSQL_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        DBSQL_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( !strcmp(fs_info.f_basetype, "qnx6") ){
      file->sector_size = fs_info.f_bsize;
      file->device_characteristics =
        DBSQL_IOCAP_ATOMIC |         /* All filesystem writes are atomic */
        DBSQL_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        DBSQL_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( !strcmp(fs_info.f_basetype, "qnx4") ){
      file->sector_size = fs_info.f_bsize;
      file->device_characteristics =
        /* full bitset of atomics from max sector size and smaller */
        (((file->sector_size / 512 * DBSQL_IOCAP_ATOMIC512) << 1) - 2) |
        DBSQL_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( strstr(fs_info.f_basetype, "dos") ){
      file->sector_size = fs_info.f_bsize;
      file->device_characteristics =
        /* full bitset of atomics from max sector size and smaller */
        (((file->sector_size / 512 * DBSQL_IOCAP_ATOMIC512) << 1) - 2) |
        DBSQL_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else{
      file->device_characteristics =
        DBSQL_IOCAP_ATOMIC512 |      /* blocks are atomic */
        DBSQL_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        0;
    }
  }
  /* Last chance verification.  If the sector size isn't a multiple of 512
  ** then it isn't valid.*/
  if( file->sector_size % 512 != 0 ){
    file->device_characteristics = 0;
    file->sector_size = DBSQL_DEFAULT_SECTOR_SIZE;
  }
}
#endif

/*
** Return the sector size in bytes of the underlying block device for
** the specified file. This is almost always 512 bytes, but may be
** larger for some devices.
**
** dbsql code assumes this function cannot fail. It also assumes that
** if two files are created in the same file-system directory (i.e.
** a database and its journal file) that the sector size will be the
** same for both.
*/
static int __unix_sector_size(dbsql_file_t *id){
  unix_file *fd = (unix_file*)id;
  __set_device_characteristics(fd);
  return fd->sector_size;
}

/*
** Return the device characteristics for the file.
**
** This VFS is set up to return DBSQL_IOCAP_POWERSAFE_OVERWRITE by default.
** However, that choice is controversial since technically the underlying
** file system does not always provide powersafe overwrites.  (In other
** words, after a power-loss event, parts of the file that were never
** written might end up being altered.)  However, non-PSOW behavior is very,
** very rare.  And asserting PSOW makes a large reduction in the amount
** of required I/O for journaling, since a lot of padding is eliminated.
**  Hence, while POWERSAFE_OVERWRITE is on by default, there is a file-control
** available to turn it off and URI query parameter available to turn it off.
*/
static int __unix_device_characteristics(dbsql_file_t *id){
  unix_file *fd = (unix_file*)id;
  __set_device_characteristics(fd);
  return fd->device_characteristics;
}

#if !defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0

/*
** Return the system page size.
**
** This function should not be called directly by other code in this file.
** Instead, it should be called via macro OS_GETPAGESIZE().
*/
static int __unix_getpagesize(void){
#if OS_VXWORKS
  return 1024;
#elif defined(_BSD_SOURCE)
  return getpagesize();
#else
  return (int)sysconf(_SC_PAGESIZE);
#endif
}

#endif /* !defined(DBSQL_OMIT_WAL) || DBSQL_MAX_MMAP_SIZE>0 */

#if !defined(DBSQL_WASI) && !defined(DBSQL_OMIT_WAL)

/*
** Object used to represent an shared memory buffer.
**
** When multiple threads all reference the same wal-index, each thread
** has its own unix_shm object, but they all point to a single instance
** of this unix_shm_node object.  In other words, each wal-index is opened
** only once per process.
**
** Each unix_shm_node object is connected to a single unix_inode_info object.
** We could coalesce this object into unix_inode_info, but that would mean
** every open file that does not use shared memory (in other words, most
** open files) would have to carry around this extra information.  So
** the unix_inode_info object contains a pointer to this unix_shm_node object
** and the unix_shm_node object is created only when needed.
**
** __unix_mutex_held() must be true when creating or destroying
** this object or while reading or writing the following fields:
**
**      n_ref
**
** The following fields are read-only after the object is created:
**
**      h_shm
**      filename
**
** Either unix_shm_node.shm_mutex must be held or unix_shm_node.n_ref==0 and
** __unix_mutex_held() is true when reading or writing any other field
** in this structure.
**
** locks[DBSQL_SHM_NLOCK]:
**   This array records the various locks held by clients on each of the
**   DBSQL_SHM_NLOCK slots. If the locks[] entry is set to 0, then no
**   locks are held by the process on this slot. If it is set to -1, then
**   some client holds an EXCLUSIVE lock on the locking slot. If the locks[]
**   value is set to a positive value, then it is the number of shared 
**   locks currently held on the slot.
**
** mutexes[DBSQL_SHM_NLOCK]:
**   Normally, when DBSQL_ENABLE_SETLK_TIMEOUT is not defined, mutex 
**   shm_mutex is used to protect the locks[] array and the right to
**   call fcntl() on unix_shm_node.h_shm to obtain or release locks.
**
**   If DBSQL_ENABLE_SETLK_TIMEOUT is defined though, we use an array
**   of mutexes - one for each locking slot. To read or write locking
**   slot locks[i_slot], the caller must hold the corresponding mutex
**   mutexes[i_slot]. Similarly, to call fcntl() to obtain or release a
**   lock corresponding to slot i_slot, mutex mutexes[i_slot] must be held.
*/
struct unix_shm_node {
  unix_inode_info *inode;     /* unix_inode_info that owns this SHM node */
  dbsql_mutex_t *shm_mutex;  /* Mutex to access this object */
  char *filename;           /* Name of the mmapped file */
  int h_shm;                  /* Open file descriptor */
  int sz_region;              /* Size of shared-memory regions */
  u_int16_t n_region;               /* Size of array regions */
  u_int8_t is_readonly;             /* True if read-only */
  u_int8_t is_unlocked;             /* True if no DMS lock held */
  char **regions;           /* Array of mapped shared-memory regions */
  int n_ref;                  /* Number of unix_shm objects pointing to this */
  unix_shm *first;           /* All unix_shm objects pointing to this */
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  dbsql_mutex_t *mutexes[DBSQL_SHM_NLOCK];
#endif
  int locks[DBSQL_SHM_NLOCK];  /* # shared locks on slot, -1==excl lock */
#ifdef DBSQL_DEBUG
  u_int8_t next_shm_id;              /* Next available unix_shm.id value */
#endif
};

/*
** Structure used internally by this VFS to record the state of an
** open shared memory connection.
**
** The following fields are initialized when this object is created and
** are read-only thereafter:
**
**    unix_shm.shm_node
**    unix_shm.id
**
** All other fields are read/write.  The unix_shm.shm_node->shm_mutex must
** be held while accessing any read/write fields.
*/
struct unix_shm {
  unix_shm_node *shm_node;     /* The underlying unix_shm_node object */
  unix_shm *next;            /* Next unix_shm with the same unix_shm_node */
  u_int8_t has_mutex;               /* True if holding the unix_shm_node->shm_mutex */
  u_int8_t id;                     /* Id of this connection within its unix_shm_node */
  u_int16_t shared_mask;            /* Mask of shared locks held */
  u_int16_t excl_mask;              /* Mask of exclusive locks held */
};

/*
** Constants used for locking
*/
#define UNIX_SHM_BASE   ((22+DBSQL_SHM_NLOCK)*4)         /* first lock byte */
#define UNIX_SHM_DMS    (UNIX_SHM_BASE+DBSQL_SHM_NLOCK)  /* deadman switch */

#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
/*
** Describe the shm object using JSON.  Used for diagnostics only.
*/
static void __unix_describe_shm(dbsql_str_t *str, unix_shm *shm){
  unix_shm_node *node = shm->shm_node;
  char lcks[16];
  dbsql_str_appendf(str, "{\"h\":%d", node->h_shm);
  assert( __unix_mutex_held() );
  dbsql_str_appendf(str, ",\"nRef\":%d", node->n_ref);
  dbsql_str_appendf(str, ",\"id\":%d", shm->id);
  dbsql_str_appendf(str, ",\"sharedMask\":%d", shm->shared_mask);
  dbsql_str_appendf(str, ",\"exclMask\":%d", shm->excl_mask);
  if( UNIX_POSIX_ADVISORY_LOCKS(node->h_shm, lcks)==DBSQL_OK ){
    dbsql_str_appendf(str, ",\"pal\":\"%s\"", lcks);
  }
  dbsql_str_append(str, "}", 1);
}
#endif /* DBSQL_DEBUG || DBSQL_ENABLE_FILESTAT */

/*
** Use F_GETLK to check whether or not there are any readers with open
** wal-mode transactions in other processes on database file file. If
** no error occurs, return DBSQL_OK and set (*pi_out) to 1 if there are
** such transactions, or 0 otherwise. If an error occurs, return an
** dbsql error code. The final value of *pi_out is undefined in this
** case.
*/
static int __unix_fcntl_external_reader(unix_file *file, int *pi_out){
  int rc = DBSQL_OK;
  *pi_out = 0;
  if( file->shm){
    unix_shm_node *shm_node = file->shm->shm_node;
    struct flock f;

    memset(&f, 0, sizeof(f));
    f.l_type = F_WRLCK;
    f.l_whence = SEEK_SET;
    f.l_start = UNIX_SHM_BASE + 3;
    f.l_len = DBSQL_SHM_NLOCK - 3;

    dbsql_mutex_enter(shm_node->shm_mutex);
    if( __os_fcntl(shm_node->h_shm, F_GETLK, &f)<0 ){
      rc = DBSQL_IOERR_LOCK;
    }else{
      *pi_out = (f.l_type!=F_UNLCK);
    }
    dbsql_mutex_leave(shm_node->shm_mutex);
  }

  return rc;
}

/*
** If file has a -shm file open and it is sharing that file with some
** other connection, either in the same process or in a separate process,
** then return true.  Return false if either file does not have a -shm
** file open or if it is the only connection to that -shm file across the
** entire system.
**
** This routine is not required for correct operation.  It can always return
** false and dbsql will continue to operate according to spec.  However,
** when this routine does its job, it adds extra robustness in cases
** where database file locks have been erroneously deleted in a WAL-mode
** database by doing close(open(DATABASE_PATHNAME)) or similar.
**
** with_t false negatives, dbsql still operates to spec, though with less
** robustness.  with_t false positives, the last database connection on a
** WAL-mode database will fail to unlink the -wal and -shm files, which
** is annoying but harmless.  False positives will also prevent a database
** connection from running "PRAGMA journal_mode=DELETE" in order to take
** the database out of WAL mode, which is perhaps more serious, but is
** still not a disaster.
*/
static int UNIX_IS_SHARING_SHM_NODE(unix_file *file){
  unix_shm_node *shm_node;
  struct flock lock;
  if( file->shm==0 ) return 0;
  if( file->ctrl_flags & UNIXFILE_EXCL ) return 0;
  shm_node = file->shm->shm_node;
#if DBSQL_ATOMIC_INTRINSICS
  assert( ATOMIC_LOAD(&shm_node->n_ref)==1 );
#endif
  memset(&lock, 0, sizeof(lock));
  lock.l_whence = SEEK_SET;
  lock.l_start = UNIX_SHM_DMS;
  lock.l_len = 1;
  lock.l_type = F_WRLCK;
  __os_fcntl(shm_node->h_shm, F_GETLK, &lock);
  return (lock.l_type!=F_UNLCK);
}

/*
** Apply posix advisory locks for all bytes from ofst through ofst+n-1.
**
** Locks block if the mask is exactly UNIX_SHM_C and are non-blocking
** otherwise.
*/
static int __unix_shm_system_lock(
  unix_file *file,       /* Open connection to the WAL file */
  int lock_type,          /* F_UNLCK, F_RDLCK, or F_WRLCK */
  int ofst,              /* First byte of the locking range */
  int n                  /* Number of bytes to lock */
){
  unix_shm_node *shm_node; /* Apply locks to this open shared-memory segment */
  struct flock f;        /* The posix advisory locking structure */
  int rc = DBSQL_OK;    /* Result code form fcntl() */

  shm_node = file->inode->shm_node;

  /* Assert that the parameters are within expected range and that the
  ** correct mutex or mutexes are held. */
  assert( shm_node->n_ref>=0 );
  assert( (ofst==UNIX_SHM_DMS && n==1)
       || (ofst>=UNIX_SHM_BASE && ofst+n<=(UNIX_SHM_BASE+DBSQL_SHM_NLOCK))
  );
  if( ofst==UNIX_SHM_DMS ){
    assert( shm_node->n_ref>0 || __unix_mutex_held() );
    assert( shm_node->n_ref==0 || dbsql_mutex_held(shm_node->shm_mutex) );
  }else{
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    int ii;
    for(ii=ofst-UNIX_SHM_BASE; ii<ofst-UNIX_SHM_BASE+n; ii++){
      assert( dbsql_mutex_held(shm_node->mutexes[ii]) );
    }
#else
    assert( dbsql_mutex_held(shm_node->shm_mutex) );
    assert( shm_node->n_ref>0 );
#endif
  }

  /* Shared locks never span more than one byte */
  assert( n==1 || lock_type!=F_RDLCK );

  /* Locks are within range */
  assert( n>=1 && n<=DBSQL_SHM_NLOCK );
  assert( ofst>=UNIX_SHM_BASE && ofst<=UNIX_SHM_DMS );
  assert( ofst+n-1<=UNIX_SHM_DMS );

  if( shm_node->h_shm>=0 ){
    int res;
    /* Initialize the locking parameters */
    f.l_type = lock_type;
    f.l_whence = SEEK_SET;
    f.l_start = ofst;
    f.l_len = n;
    res = OS_SET_POSIX_ADVISORY_LOCK(shm_node->h_shm, &f, file);
    if( res==-1 ){
#if defined(DBSQL_ENABLE_SETLK_TIMEOUT) && DBSQL_ENABLE_SETLK_TIMEOUT==1
      rc = (file->i_busy_timeout ? DBSQL_BUSY_TIMEOUT : DBSQL_BUSY);
#else
      rc = DBSQL_BUSY;
#endif
    }
  }

  /* Do debug tracing */
#ifdef DBSQL_DEBUG
  OSTRACE(("SHM-LOCK "));
  if( rc==DBSQL_OK ){
    if( lock_type==F_UNLCK ){
      OSTRACE(("unlock %d..%d ok\n", ofst, ofst+n-1));
    }else if( lock_type==F_RDLCK ){
      OSTRACE(("read-lock %d..%d ok\n", ofst, ofst+n-1));
    }else{
      assert( lock_type==F_WRLCK );
      OSTRACE(("write-lock %d..%d ok\n", ofst, ofst+n-1));
    }
  }else{
    if( lock_type==F_UNLCK ){
      OSTRACE(("unlock %d..%d failed\n", ofst, ofst+n-1));
    }else if( lock_type==F_RDLCK ){
      OSTRACE(("read-lock %d..%d failed\n", ofst, ofst+n-1));
    }else{
      assert( lock_type==F_WRLCK );
      OSTRACE(("write-lock %d..%d failed\n", ofst, ofst+n-1));
    }
  }
#endif

  return rc;
}

/*
** Return the minimum number of 32KB shm regions that should be mapped at
** a time, assuming that each mapping must be an integer multiple of the
** current system page-size.
**
** Usually, this is 1. The exception seems to be systems that are configured
** to use 64KB pages - in this case each mapping must cover at least two
** shm regions.
*/
static int __unix_shm_region_per_map(void){
  int shmsz = 32*1024;            /* SHM region size */
  int pgsz = OS_GETPAGESIZE();   /* System page size */
  assert( ((pgsz-1)&pgsz)==0 );   /* Page size must be a power of 2 */
  if( pgsz<shmsz ) return 1;
  return pgsz/shmsz;
}

/*
** Purge the unix_shm_node_list list of all entries with unix_shm_node.n_ref==0.
**
** This is not a VFS shared-memory method; it is a utility function called
** by VFS shared-memory methods.
*/
static void __unix_shm_purge(unix_file *fd){
  unix_shm_node *p = fd->inode->shm_node;
  assert( __unix_mutex_held() );
  if( p && ALWAYS(p->n_ref==0) ){
    int n_shm_per_map = __unix_shm_region_per_map();
    int i;
    assert( p->inode==fd->inode );
    dbsql_mutex_free(p->shm_mutex);
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    for(i=0; i<DBSQL_SHM_NLOCK; i++){
      dbsql_mutex_free(p->mutexes[i]);
    }
#endif
    for(i=0; i<p->n_region; i+=n_shm_per_map){
      if( p->h_shm>=0 ){
        OS_MUNMAP(p->regions[i], p->sz_region);
      }else{
        dbsql_free(p->regions[i]);
      }
    }
    dbsql_free(p->regions);
    if( p->h_shm>=0 ){
      robust_close(fd, p->h_shm, __LINE__);
      p->h_shm = -1;
    }
    p->inode->shm_node = 0;
    dbsql_free(p);
  }
}

/*
** The DMS lock has not yet been taken on shm file shm_node. Attempt to
** take it now. Return DBSQL_OK if successful, or an dbsql error
** code otherwise.
**
** If the DMS cannot be locked because this is a readonly_shm=1
** connection and no other process already holds a lock, return
** DBSQL_READONLY_CANTINIT and set shm_node->is_unlocked=1.
*/
static int __unix_lock_shared_memory(unix_file *db_fd, unix_shm_node *shm_node){
  struct flock lock;
  int rc = DBSQL_OK;

  /* Use F_GETLK to determine the locks other processes are holding
  ** on the DMS byte. If it indicates that another process is holding
  ** a SHARED lock, then this process may also take a SHARED lock
  ** and proceed with opening the *-shm file.
  **
  ** Or, if no other process is holding any lock, then this process
  ** is the first to open it. In this case take an EXCLUSIVE lock on the
  ** DMS byte and truncate the *-shm file to zero bytes in size. Then
  ** downgrade to a SHARED lock on the DMS byte.
  **
  ** If another process is holding an EXCLUSIVE lock on the DMS byte,
  ** return DBSQL_BUSY to the caller (it will try again). An earlier
  ** version of this code attempted the SHARED lock at this point. But
  ** this introduced a subtle race condition: if the process holding
  ** EXCLUSIVE failed just before truncating the *-shm file, then this
  ** process might open and use the *-shm file without truncating it.
  ** And if the *-shm file has been corrupted by a power failure or
  ** system crash, the database itself may also become corrupt.  */
  lock.l_whence = SEEK_SET;
  lock.l_start = UNIX_SHM_DMS;
  lock.l_len = 1;
  lock.l_type = F_WRLCK;
  if( __os_fcntl(shm_node->h_shm, F_GETLK, &lock)!=0 ) {
    rc = DBSQL_IOERR_LOCK;
  }else if( lock.l_type==F_UNLCK ){
    if( shm_node->is_readonly ){
      shm_node->is_unlocked = 1;
      rc = DBSQL_READONLY_CANTINIT;
    }else{
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
      /* Do not use a blocking lock here. If the lock cannot be obtained
      ** immediately, it means some other connection is truncating the
      ** *-shm file. And after it has done so, it will not release its
      ** lock, but only downgrade it to a shared lock. So no point in
      ** blocking here. The call below to obtain the shared DMS lock may 
      ** use a blocking lock. */
      int i_save_timeout = db_fd->i_busy_timeout;
      db_fd->i_busy_timeout = 0;
#endif
      rc = __unix_shm_system_lock(db_fd, F_WRLCK, UNIX_SHM_DMS, 1);
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
      db_fd->i_busy_timeout = i_save_timeout;
#endif
      /* The first connection to attach must truncate the -shm file.  We
      ** truncate to 3 bytes (an arbitrary small number, less than the
      ** -shm header size) rather than 0 as a system debugging aid, to
      ** help detect if a -shm file truncation is legitimate or is the work
      ** or a rogue process. */
      if( rc==DBSQL_OK && robust_ftruncate(shm_node->h_shm, 3) ){
        rc = UNIX_LOG_ERROR(DBSQL_IOERR_SHMOPEN,"ftruncate",shm_node->filename);
      }
    }
  }else if( lock.l_type==F_WRLCK ){
    rc = DBSQL_BUSY;
  }

  if( rc==DBSQL_OK ){
    assert( lock.l_type==F_UNLCK || lock.l_type==F_RDLCK );
    rc = __unix_shm_system_lock(db_fd, F_RDLCK, UNIX_SHM_DMS, 1);
  }
  return rc;
}

/*
** Open a shared-memory area associated with open database file db_fd.
** This particular implementation uses mmapped files.
**
** The file used to implement shared-memory is in the same directory
** as the open database file and has the same name as the open database
** file with the "-shm" suffix added.  For example, if the database file
** is "/home/user1/config.db" then the file that is created and mmapped
** for shared memory will be called "/home/user1/config.db-shm".
**
** Another approach to is to use files in /dev/shm or /dev/tmp or an
** some other tmpfs mount. But if a file in a different directory
** from the database file is used, then differing access permissions
** or a chroot() might cause two different processes on the same
** database to end up using different files for shared memory -
** meaning that their memory would not really be shared - resulting
** in database corruption.  Nevertheless, this tmpfs file usage
** can be enabled at compile-time using -DSQLITE_SHM_DIRECTORY="/dev/shm"
** or the equivalent.  The use of the DBSQL_SHM_DIRECTORY compile-time
** option results in an incompatible build of dbsql;  builds of dbsql
** that with differing DBSQL_SHM_DIRECTORY settings attempt to use the
** same database file at the same time, database corruption will likely
** result. The DBSQL_SHM_DIRECTORY compile-time option is considered
** "unsupported" and may go away in a future dbsql release.
**
** When opening a new shared-memory file, if no other instances of that
** file are currently open, in this process or in other processes, then
** the file must be truncated to zero length or have its header cleared.
**
** If the original database file (db_fd) is using the "unix-excl" VFS
** that means that an exclusive lock is held on the database file and
** that no other processes are able to read or write the database.  In
** that case, we do not really need shared memory.  No shared memory
** file is created.  The shared memory will be simulated with heap memory.
*/
static int __unix_open_shared_memory(unix_file *db_fd){
  struct unix_shm *p = 0;          /* The connection to be opened */
  struct unix_shm_node *shm_node;   /* The underlying mmapped file */
  int rc = DBSQL_OK;             /* Result code */
  unix_inode_info *inode;          /* The inode of fd */
  char *shm;             /* Name of the file used for SHM */
  int n_shm_filename;               /* Size of the SHM filename in bytes */

  /* Allocate space for the new unix_shm object. */
  p = dbsql_malloc64( sizeof(*p) );
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  memset(p, 0, sizeof(*p));
  assert( db_fd->shm==0 );

  /* Check to see if a unix_shm_node object already exists. Reuse an existing
  ** one if present. Create a new one if necessary.
  */
  assert( __unix_file_mutex_notheld(db_fd) );
  __unix_enter_mutex();
  inode = db_fd->inode;
  shm_node = inode->shm_node;
  if( shm_node==0 ){
    struct stat s_stat;                 /* fstat() info for database file */
#ifndef DBSQL_SHM_DIRECTORY
    const char *base_path = db_fd->path;
#endif

    /* Call fstat() to figure out the permissions on the database file. If
    ** a new *-shm file is created, an attempt will be made to create it
    ** with the same permissions.
    */
    if( OS_FSTAT(db_fd->h, &s_stat) ){
      rc = DBSQL_IOERR_FSTAT;
      goto shm_open_err;
    }

#ifdef DBSQL_SHM_DIRECTORY
    n_shm_filename = sizeof(DBSQL_SHM_DIRECTORY) + 31;
#else
    n_shm_filename = 6 + (int)strlen(base_path);
#endif
    shm_node = dbsql_malloc64( sizeof(*shm_node) + n_shm_filename );
    if( shm_node==0 ){
      rc = DBSQL_NOMEM_BKPT;
      goto shm_open_err;
    }
    memset(shm_node, 0, sizeof(*shm_node)+n_shm_filename);
    shm = shm_node->filename = (char*)&shm_node[1];
#ifdef DBSQL_SHM_DIRECTORY
    dbsql_snprintf(n_shm_filename, shm,
                     DBSQL_SHM_DIRECTORY "/dbsql-shm-%x-%x",
                     (u_int32_t)s_stat.st_ino, (u_int32_t)s_stat.st_dev);
#else
    dbsql_snprintf(n_shm_filename, shm, "%s-shm", base_path);
    __file_suffix3(db_fd->path, shm);
#endif
    shm_node->h_shm = -1;
    db_fd->inode->shm_node = shm_node;
    shm_node->inode = db_fd->inode;
    if( __global_config.core_mutex ){
      shm_node->shm_mutex = dbsql_mutex_alloc(DBSQL_MUTEX_FAST);
      if( shm_node->shm_mutex==0 ){
        rc = DBSQL_NOMEM_BKPT;
        goto shm_open_err;
      }
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
      {
        int ii;
        for(ii=0; ii<DBSQL_SHM_NLOCK; ii++){
          shm_node->mutexes[ii] = dbsql_mutex_alloc(DBSQL_MUTEX_FAST);
          if( shm_node->mutexes[ii]==0 ){
            rc = DBSQL_NOMEM_BKPT;
            goto shm_open_err;
          }
        }
      }
#endif
    }

    if( inode->process_lock==0 ){
      if( 0==dbsql_uri_boolean(db_fd->path, "readonly_shm", 0) ){
        shm_node->h_shm = robust_open(shm, O_RDWR|O_CREAT|O_NOFOLLOW,
                                     (s_stat.st_mode&0777));
      }
      if( shm_node->h_shm<0 ){
        shm_node->h_shm = robust_open(shm, O_RDONLY|O_NOFOLLOW,
                                     (s_stat.st_mode&0777));
        if( shm_node->h_shm<0 ){
          rc = UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "open", shm);
          goto shm_open_err;
        }
        shm_node->is_readonly = 1;
      }

      /* If this process is running as root, make sure that the SHM file
      ** is owned by the same user that owns the original database.  Otherwise,
      ** the original owner will not be able to connect.
      */
      __robust_fchown(shm_node->h_shm, s_stat.st_uid, s_stat.st_gid);

      rc = __unix_lock_shared_memory(db_fd, shm_node);
      if( rc!=DBSQL_OK && rc!=DBSQL_READONLY_CANTINIT ) goto shm_open_err;
    }
  }

  /* Make the new connection a child of the unix_shm_node */
  p->shm_node = shm_node;
#ifdef DBSQL_DEBUG
  p->id = shm_node->next_shm_id++;
#endif
  shm_node->n_ref++;
  db_fd->shm = p;
  __unix_leave_mutex();

  /* The reference count on shm_node has already been incremented under
  ** the cover of the __unix_enter_mutex() mutex and the pointer from the
  ** new (struct unix_shm) object to the shm_node has been set. All that is
  ** left to do is to link the new object into the linked list starting
  ** at shm_node->first. This must be done while holding the
  ** shm_node->shm_mutex.
  */
  dbsql_mutex_enter(shm_node->shm_mutex);
  p->next = shm_node->first;
  shm_node->first = p;
  dbsql_mutex_leave(shm_node->shm_mutex);
  return rc;

  /* Jump here on any error */
shm_open_err:
  __unix_shm_purge(db_fd);       /* This call frees shm_node if required */
  dbsql_free(p);
  __unix_leave_mutex();
  return rc;
}

/*
** This function is called to obtain a pointer to region i_region of the
** shared-memory associated with the database file fd. Shared-memory regions
** are numbered starting from zero. Each shared-memory region is sz_region
** bytes in size.
**
** If an error occurs, an error code is returned and *pp is set to NULL.
**
** Otherwise, if the extend parameter is 0 and the requested shared-memory
** region has not been allocated (by any client, including one running in a
** separate process), then *pp is set to NULL and DBSQL_OK returned. If
** extend is non-zero and the requested shared-memory region has not yet
** been allocated, it is allocated by this function.
**
** If the shared-memory region has already been allocated or is allocated by
** this call as described above, then it is mapped into this processes
** address space (if it is not already), *pp is set to point to the mapped
** memory and DBSQL_OK returned.
*/
static int UNIX_SHM_MAP(
  dbsql_file_t *fd,               /* Handle open on database file */
  int i_region,                    /* Region to retrieve */
  int sz_region,                   /* Size of regions */
  int extend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  unix_file *db_fd = (unix_file*)fd;
  unix_shm *p;
  unix_shm_node *shm_node;
  int rc = DBSQL_OK;
  int n_shm_per_map = __unix_shm_region_per_map();
  int n_req_region;

  /* If the shared-memory file has not yet been opened, open it now. */
  if( db_fd->shm==0 ){
    rc = __unix_open_shared_memory(db_fd);
    if( rc!=DBSQL_OK ) return rc;
  }

  p = db_fd->shm;
  shm_node = p->shm_node;
  dbsql_mutex_enter(shm_node->shm_mutex);
  if( shm_node->is_unlocked ){
    rc = __unix_lock_shared_memory(db_fd, shm_node);
    if( rc!=DBSQL_OK ) goto shmpage_out;
    shm_node->is_unlocked = 0;
  }
  assert( sz_region==shm_node->sz_region || shm_node->n_region==0 );
  assert( shm_node->inode==db_fd->inode );
  assert( shm_node->h_shm>=0 || db_fd->inode->process_lock==1 );
  assert( shm_node->h_shm<0 || db_fd->inode->process_lock==0 );

  /* Minimum number of regions required to be mapped. */
  n_req_region = ((i_region+n_shm_per_map) / n_shm_per_map) * n_shm_per_map;

  if( shm_node->n_region<n_req_region ){
    char **news;                         /* New regions[] array */
    int64_t n_byte = n_req_region*(int64_t)sz_region; /* Minimum required file size */
    struct stat s_stat;                    /* Used by fstat() */

    shm_node->sz_region = sz_region;

    if( shm_node->h_shm>=0 ){
      /* The requested region is not mapped into this processes address space.
      ** Check to see if it has been allocated (i.e. if the wal-index file is
      ** large enough to contain the requested region).
      */
      if( OS_FSTAT(shm_node->h_shm, &s_stat) ){
        rc = DBSQL_IOERR_SHMSIZE;
        goto shmpage_out;
      }

      if( s_stat.st_size<n_byte ){
        /* The requested memory region does not exist. If extend is set to
        ** false, exit early. *pp will be set to NULL and DBSQL_OK returned.
        */
        if( !extend ){
          goto shmpage_out;
        }

        /* Alternatively, if extend is true, extend the file. Do this by
        ** writing a single byte to the end of each (OS) page being
        ** allocated or extended. Technically, we need only write to the
        ** last page in order to extend the file. But writing to all new
        ** pages forces the OS to allocate them immediately, which reduces
        ** the chances of SIGBUS while accessing the mapped region later on.
        */
        else{
          static const int pgsz = 4096;
          int64_t i_pg;

          /* Write to the last byte of each newly allocated or extended page */
          assert( (n_byte % pgsz)==0 );
          for(i_pg=(s_stat.st_size/pgsz); i_pg<(n_byte/pgsz); i_pg++){
            int x = 0;
            if( __seek_and_write_fd(shm_node->h_shm, i_pg*pgsz + pgsz-1,"",1,&x)!=1 ){
              const char *file = shm_node->filename;
              rc = UNIX_LOG_ERROR(DBSQL_IOERR_SHMSIZE, "write", file);
              goto shmpage_out;
            }
          }
        }
      }
    }

    /* Map the requested memory region into this processes address space. */
    news = (char **)dbsql_realloc64(
        shm_node->regions, n_req_region*sizeof(char *)
    );
    if( !news ){
      rc = DBSQL_IOERR_NOMEM_BKPT;
      goto shmpage_out;
    }
    shm_node->regions = news;
    while( shm_node->n_region<n_req_region ){
      int64_t n_map = (int64_t)sz_region*(int64_t)n_shm_per_map;
      int64_t i;
      void *mem;
      if( shm_node->h_shm>=0 ){
        mem = OS_MMAP(0, n_map,
            shm_node->is_readonly ? PROT_READ : PROT_READ|PROT_WRITE,
            MAP_SHARED, shm_node->h_shm, sz_region*(int64_t)shm_node->n_region
        );
        if( mem==MAP_FAILED ){
          rc = UNIX_LOG_ERROR(DBSQL_IOERR_SHMMAP, "mmap", shm_node->filename);
          goto shmpage_out;
        }
      }else{
        mem = dbsql_malloc64(n_map);
        if( mem==0 ){
          rc = DBSQL_NOMEM_BKPT;
          goto shmpage_out;
        }
        memset(mem, 0, n_map);
      }

      for(i=0; i<n_shm_per_map; i++){
        shm_node->regions[shm_node->n_region+i] = &((char*)mem)[sz_region*i];
      }
      shm_node->n_region += n_shm_per_map;
    }
  }

shmpage_out:
  if( shm_node->n_region>i_region ){
    *pp = shm_node->regions[i_region];
  }else{
    *pp = 0;
  }
  if( shm_node->is_readonly && rc==DBSQL_OK ) rc = DBSQL_READONLY;
  dbsql_mutex_leave(shm_node->shm_mutex);
  return rc;
}

/*
** Check that the shm_node->locks[] array comports with the locking bitmasks
** held by each client. Return true if it does, or false otherwise. This
** is to be used in an assert(). e.g.
**
**     assert( __assert_locking_array_ok(shm_node) );
*/
#ifdef DBSQL_DEBUG
static int __assert_locking_array_ok(unix_shm_node *shm_node){
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  return 1;
#else
  unix_shm *x;
  int locks[DBSQL_SHM_NLOCK];

  memset(locks, 0, sizeof(locks));
  for(x=shm_node->first; x; x=x->next){
    int i;
    for(i=0; i<DBSQL_SHM_NLOCK; i++){
      if( x->excl_mask & (1<<i) ){
        assert( locks[i]==0 );
        locks[i] = -1;
      }else if( x->shared_mask & (1<<i) ){
        assert( locks[i]>=0 );
        locks[i]++;
      }
    }
  }

  assert( 0==memcmp(shm_node->locks, locks, sizeof(locks)) );
  return (memcmp(shm_node->locks, locks, sizeof(locks))==0);
#endif
}
#endif /* !defined(DBSQL_WASI) && !defined(DBSQL_OMIT_WAL) */

/*
** Change the lock state for a shared-memory segment.
**
** Note that the relationship between SHARED and EXCLUSIVE locks is a little
** different here than in posix.  In shm_lock(), one can go from unlocked
** to shared and back or from unlocked to exclusive and back.  But one may
** not go from shared to exclusive or from exclusive to shared.
*/
static int UNIX_SHM_LOCK(
  dbsql_file_t *fd,          /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  unix_file *db_fd = (unix_file*)fd;      /* Connection holding shared memory */
  unix_shm *p;                           /* The shared memory being locked */
  unix_shm_node *shm_node;                /* The underlying file i_node */
  int rc = DBSQL_OK;                   /* Result code */
  u_int16_t mask = (1<<(ofst+n)) - (1<<ofst); /* Mask of locks to take or release */
  int *locks;

  p = db_fd->shm;
  if( p==0 ) return DBSQL_IOERR_SHMLOCK;
  shm_node = p->shm_node;
  if( NEVER(shm_node==0) ) return DBSQL_IOERR_SHMLOCK;
  locks = shm_node->locks;

  assert( shm_node==db_fd->inode->shm_node );
  assert( shm_node->inode==db_fd->inode );
  assert( ofst>=0 && ofst+n<=DBSQL_SHM_NLOCK );
  assert( n>=1 );
  assert( flags==(DBSQL_SHM_LOCK | DBSQL_SHM_SHARED)
       || flags==(DBSQL_SHM_LOCK | DBSQL_SHM_EXCLUSIVE)
       || flags==(DBSQL_SHM_UNLOCK | DBSQL_SHM_SHARED)
       || flags==(DBSQL_SHM_UNLOCK | DBSQL_SHM_EXCLUSIVE) );
  assert( n==1 || (flags & DBSQL_SHM_EXCLUSIVE)!=0 );
  assert( shm_node->h_shm>=0 || db_fd->inode->process_lock==1 );
  assert( shm_node->h_shm<0 || db_fd->inode->process_lock==0 );

  /* Check that, if this to be a blocking lock, no locks that occur later
  ** in the following list than the lock being obtained are already held:
  **
  **   1. Recovery lock (ofst==2).
  **   2. Checkpointer lock (ofst==1).
  **   3. Write lock (ofst==0).
  **   4. Read locks (ofst>=3 && ofst<DBSQL_SHM_NLOCK).
  **
  ** In other words, if this is a blocking lock, none of the locks that
  ** occur later in the above list than the lock being obtained may be
  ** held.
  */
#if defined(DBSQL_ENABLE_SETLK_TIMEOUT) && defined(DBSQL_DEBUG)
  {
    u_int16_t lock_mask = (p->excl_mask|p->shared_mask);
    assert( (flags & DBSQL_SHM_UNLOCK) || db_fd->i_busy_timeout==0 || (
          (ofst!=2 || lock_mask==0)
       && (ofst!=1 || lock_mask==0 || lock_mask==2)
       && (ofst!=0 || lock_mask<3)
       && (ofst<3  || lock_mask<(1<<ofst))
    ));
  }
#endif

  /* Check if there is any work to do. There are three cases:
  **
  **    a) An unlock operation where there are locks to unlock,
  **    b) An shared lock where the requested lock is not already held
  **    c) An exclusive lock where the requested lock is not already held
  **
  ** The dbsql core never requests an exclusive lock that it already holds.
  ** This is assert()ed below.
  */
  assert( flags!=(DBSQL_SHM_EXCLUSIVE|DBSQL_SHM_LOCK) 
       || 0==(p->excl_mask & mask) 
  );
  if( ((flags & DBSQL_SHM_UNLOCK) && ((p->excl_mask|p->shared_mask) & mask))
   || (flags==(DBSQL_SHM_SHARED|DBSQL_SHM_LOCK) && 0==(p->shared_mask & mask))
   || (flags==(DBSQL_SHM_EXCLUSIVE|DBSQL_SHM_LOCK))
  ){

    /* Take the required mutexes. In SETLK_TIMEOUT mode (blocking locks), if
    ** this is an attempt on an exclusive lock use dbsql_mutex_try(). If any
    ** other thread is holding this mutex, then it is either holding or about
    ** to hold a lock exclusive to the one being requested, and we may
    ** therefore return DBSQL_BUSY to the caller.
    **
    ** Doing this prevents some deadlock scenarios. For example, thread 1 may
    ** be a checkpointer blocked waiting on the WRITER lock. And thread 2
    ** may be a normal SQL client upgrading to a write transaction. In this
    ** case thread 2 does a non-blocking request for the WRITER lock. But -
    ** if it were to use dbsql_mutex_enter() then it would effectively
    ** become a (doomed) blocking request, as thread 2 would block until thread
    ** 1 obtained WRITER and released the mutex. Since thread 2 already holds
    ** a lock on a read-locking slot at this point, this breaks the
    ** anti-deadlock rules (see above).  */
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    int i_mutex;
    for(i_mutex=ofst; i_mutex<ofst+n; i_mutex++){
      if( flags==(DBSQL_SHM_LOCK|DBSQL_SHM_EXCLUSIVE) ){
        rc = dbsql_mutex_try(shm_node->mutexes[i_mutex]);
        if( rc!=DBSQL_OK ) goto leave_shmnode_mutexes;
      }else{
        dbsql_mutex_enter(shm_node->mutexes[i_mutex]);
      }
    }
#else
    dbsql_mutex_enter(shm_node->shm_mutex);
#endif

    if( ALWAYS(rc==DBSQL_OK) ){
      if( flags & DBSQL_SHM_UNLOCK ){
        /* Case (a) - unlock.  */
        int unlock = 1;
        assert( (p->excl_mask & p->shared_mask)==0 );
        assert( !(flags & DBSQL_SHM_EXCLUSIVE) || (p->excl_mask & mask)==mask );
        assert( !(flags & DBSQL_SHM_SHARED) || (p->shared_mask & mask)==mask );
  
        /* If this is a SHARED lock being unlocked, it is possible that other
        ** clients within this process are holding the same SHARED lock. In
        ** this case, set unlock to 0 so that the posix lock is not removed
        ** from the file-descriptor below.  */
        if( flags & DBSQL_SHM_SHARED ){
          assert( n==1 );
          assert( locks[ofst]>=1 );
          if( locks[ofst]>1 ){
            unlock = 0;
            locks[ofst]--;
            p->shared_mask &= ~mask;
          }
        }
  
        if( unlock ){
          rc = __unix_shm_system_lock(db_fd, F_UNLCK, ofst+UNIX_SHM_BASE, n);
          if( rc==DBSQL_OK ){
            memset(&locks[ofst], 0, sizeof(int)*n);
            p->shared_mask &= ~mask;
            p->excl_mask &= ~mask;
          }
        }
      }else if( flags & DBSQL_SHM_SHARED ){
        /* Case (b) - a shared lock.  */
  
        if( locks[ofst]<0 ){
          /* An exclusive lock is held by some other connection. BUSY. */ 
          rc = DBSQL_BUSY;
        }else if( locks[ofst]==0 ){
          rc = __unix_shm_system_lock(db_fd, F_RDLCK, ofst+UNIX_SHM_BASE, n);
        }
  
        /* Get the local shared locks */
        if( rc==DBSQL_OK ){
          p->shared_mask |= mask;
          locks[ofst]++;
        }
      }else{
        /* Case (c) - an exclusive lock.  */
        int ii;
  
        assert( flags==(DBSQL_SHM_LOCK|DBSQL_SHM_EXCLUSIVE) );
        assert( (p->shared_mask & mask)==0 );
        assert( (p->excl_mask & mask)==0 );
  
        /* Make sure no sibling connections hold locks that will block this
        ** lock.  If any do, return DBSQL_BUSY right away.  */
        for(ii=ofst; ii<ofst+n; ii++){
          if( locks[ii] ){
            rc = DBSQL_BUSY;
            break;
          }
        }
  
        /* Get the exclusive locks at the system level. Then if successful
        ** also update the in-memory values. */
        if( rc==DBSQL_OK ){
          rc = __unix_shm_system_lock(db_fd, F_WRLCK, ofst+UNIX_SHM_BASE, n);
          if( rc==DBSQL_OK ){
            p->excl_mask |= mask;
            for(ii=ofst; ii<ofst+n; ii++){
              locks[ii] = -1;
            }
          }
        }
      }
      assert( __assert_locking_array_ok(shm_node) );
    }

    /* Drop the mutexes acquired above. */
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  leave_shmnode_mutexes:
    for(i_mutex--; i_mutex>=ofst; i_mutex--){
      dbsql_mutex_leave(shm_node->mutexes[i_mutex]);
    }
#else
    dbsql_mutex_leave(shm_node->shm_mutex);
#endif
  }

  OSTRACE(("SHM-LOCK shmid-%d, pid-%d got %03x,%03x\n",
           p->id, OS_GETPID(0), p->shared_mask, p->excl_mask));
  return rc;
}

/*
** Implement a memory barrier or memory fence on shared memory.
**
** All loads and stores begun before the barrier must complete before
** any load or store begun after the barrier.
*/
static void UNIX_SHM_BARRIER(
  dbsql_file_t *fd                /* Database file holding the shared memory */
){
  UNUSED_PARAMETER(fd);
  __memory_barrier();         /* compiler-defined memory barrier */
  assert( fd->methods->lock==__nolock_lock
       || __unix_file_mutex_notheld((unix_file*)fd)
  );
  __unix_enter_mutex();               /* Also mutex, for redundancy */
  __unix_leave_mutex();
}

/*
** Close a connection to shared-memory.  Delete the underlying
** storage if delete_flag is true.
**
** If there is no shared memory associated with the connection then this
** routine is a harmless no-op.
*/
static int UNIX_SHM_UNMAP(
  dbsql_file_t *fd,               /* The underlying database file */
  int delete_flag                  /* Delete shared-memory if true */
){
  unix_shm *p;                     /* The connection to be closed */
  unix_shm_node *shm_node;          /* The underlying shared-memory file */
  unix_shm **pp;                   /* For looping over sibling connections */
  unix_file *db_fd;                /* The underlying database file */

  db_fd = (unix_file*)fd;
  p = db_fd->shm;
  if( p==0 ) return DBSQL_OK;
  shm_node = p->shm_node;

  assert( shm_node==db_fd->inode->shm_node );
  assert( shm_node->inode==db_fd->inode );

  /* Remove connection p from the set of connections associated
  ** with shm_node */
  dbsql_mutex_enter(shm_node->shm_mutex);
  for(pp=&shm_node->first; (*pp)!=p; pp = &(*pp)->next){}
  *pp = p->next;

  /* Free the connection p */
  dbsql_free(p);
  db_fd->shm = 0;
  dbsql_mutex_leave(shm_node->shm_mutex);

  /* If shm_node->n_ref has reached 0, then close the underlying
  ** shared-memory file, too */
  assert( __unix_file_mutex_notheld(db_fd) );
  __unix_enter_mutex();
  assert( shm_node->n_ref>0 );
  shm_node->n_ref--;
  if( shm_node->n_ref==0 ){
    if( delete_flag && shm_node->h_shm>=0 ){
      OS_UNLINK(shm_node->filename);
    }
    __unix_shm_purge(db_fd);
  }
  __unix_leave_mutex();

  return DBSQL_OK;
}


#else
# define UNIX_SHM_MAP     0
# define UNIX_SHM_LOCK    0
# define UNIX_SHM_BARRIER 0
# define UNIX_SHM_UNMAP   0
#endif /* #ifndef DBSQL_OMIT_WAL */

#if DBSQL_MAX_MMAP_SIZE>0
/*
** If it is currently memory mapped, unmap file fd.
*/
static void __unix_unmapfile(unix_file *fd){
  assert( fd->n_fetch_out==0 );
  if( fd->map_region ){
    OS_MUNMAP(fd->map_region, fd->mmap_size_actual);
    fd->map_region = 0;
    fd->mmap_size = 0;
    fd->mmap_size_actual = 0;
  }
}

/*
** Attempt to set the size of the memory mapping maintained by file
** descriptor fd to n_new bytes. Any existing mapping is discarded.
**
** If successful, this function sets the following variables:
**
**       unix_file.map_region
**       unix_file.mmap_size
**       unix_file.mmap_size_actual
**
** If unsuccessful, an error message is logged via dbsql_log() and
** the three variables above are zeroed. In this case dbsql should
** continue accessing the database using the x_read() and x_write()
** methods.
*/
static void __unix_remapfile(
  unix_file *fd,                  /* File descriptor object */
  int64_t n_new                        /* Required mapping size */
){
  const char *err = "mmap";
  int h = fd->h;                      /* File descriptor open on db file */
  u_int8_t *orig = (u_int8_t *)fd->map_region;   /* Pointer to current file mapping */
  int64_t n_orig = fd->mmap_size_actual;     /* Size of orig region in bytes */
  u_int8_t *p_new = 0;                        /* Location of new mapping */
  int flags = PROT_READ;               /* Flags to pass to mmap() */

  assert( fd->n_fetch_out==0 );
  assert( n_new>fd->mmap_size );
  assert( n_new<=fd->mmap_size_max );
  assert( n_new>0 );
  assert( fd->mmap_size_actual>=fd->mmap_size );
  assert( MAP_FAILED!=0 );

#ifdef DBSQL_MMAP_READWRITE
  if( (fd->ctrl_flags & UNIXFILE_RDONLY)==0 ) flags |= PROT_WRITE;
#endif

  if( orig ){
#if HAVE_MREMAP
    int64_t n_reuse = fd->mmap_size;
#else
    const int sz_syspage = OS_GETPAGESIZE();
    int64_t n_reuse = (fd->mmap_size & ~(sz_syspage-1));
#endif
    u_int8_t *req = &orig[n_reuse];

    /* Unmap any pages of the existing mapping that cannot be reused. */
    if( n_reuse!=n_orig ){
      OS_MUNMAP(req, n_orig-n_reuse);
    }

#if HAVE_MREMAP
    p_new = OS_MREMAP(orig, n_reuse, n_new, MREMAP_MAYMOVE);
    err = "mremap";
#else
    p_new = OS_MMAP(req, n_new-n_reuse, flags, MAP_SHARED, h, n_reuse);
    if( p_new!=MAP_FAILED ){
      if( p_new!=req ){
        OS_MUNMAP(p_new, n_new - n_reuse);
        p_new = 0;
      }else{
        p_new = orig;
      }
    }
#endif

    /* The attempt to extend the existing mapping failed. Free it. */
    if( p_new==MAP_FAILED || p_new==0 ){
      OS_MUNMAP(orig, n_reuse);
    }
  }

  /* If p_new is still NULL, try to create an entirely new mapping. */
  if( p_new==0 ){
    p_new = OS_MMAP(0, n_new, flags, MAP_SHARED, h, 0);
  }

  if( p_new==MAP_FAILED ){
    p_new = 0;
    n_new = 0;
    UNIX_LOG_ERROR(DBSQL_OK, err, fd->path);

    /* If the mmap() above failed, assume that all subsequent mmap() calls
    ** will probably fail too. Fall back to using x_read/x_write exclusively
    ** in this case.  */
    fd->mmap_size_max = 0;
  }
  fd->map_region = (void *)p_new;
  fd->mmap_size = fd->mmap_size_actual = n_new;
}

/*
** Memory map or remap the file opened by file-descriptor fd (if the file
** is already mapped, the existing mapping is replaced by the new). Or, if
** there already exists a mapping for this file, and there are still
** outstanding fetch() references to it, this function is a no-op.
**
** If parameter n_byte is non-negative, then it is the requested size of
** the mapping to create. Otherwise, if n_byte is less than zero, then the
** requested size is the size of the file on disk. The actual size of the
** created mapping is either the requested size or the value configured
** using DBSQL_FCNTL_MMAP_LIMIT, whichever is smaller.
**
** DBSQL_OK is returned if no error occurs (even if the mapping is not
** recreated as a result of outstanding references) or an dbsql error
** code otherwise.
*/
static int __unix_mapfile(unix_file *fd, int64_t n_map){
  assert( n_map>=0 || fd->n_fetch_out==0 );
  assert( n_map>0 || (fd->mmap_size==0 && fd->map_region==0) );
  if( fd->n_fetch_out>0 ) return DBSQL_OK;

  if( n_map<0 ){
    struct stat statbuf;          /* Low-level file information */
    if( OS_FSTAT(fd->h, &statbuf) ){
      return DBSQL_IOERR_FSTAT;
    }
    n_map = statbuf.st_size;
  }
  if( n_map>fd->mmap_size_max ){
    n_map = fd->mmap_size_max;
  }

  assert( n_map>0 || (fd->mmap_size==0 && fd->map_region==0) );
  if( n_map!=fd->mmap_size ){
    __unix_remapfile(fd, n_map);
  }

  return DBSQL_OK;
}
#endif /* DBSQL_MAX_MMAP_SIZE>0 */

/*
** If possible, return a pointer to a mapping of file fd starting at offset
** i_off. The mapping must be valid for at least n_amt bytes.
**
** If such a pointer can be obtained, store it in *pp and return DBSQL_OK.
** Or, if one cannot but no error occurs, set *pp to 0 and return DBSQL_OK.
** Finally, if an error does occur, return an dbsql error code. The final
** value of *pp is undefined in this case.
**
** If this function does return a pointer, the caller must eventually
** release the reference by calling __unix_unfetch().
*/
static int __unix_fetch(dbsql_file_t *fd, int64_t i_off, int n_amt, void **pp){
#if DBSQL_MAX_MMAP_SIZE>0
  unix_file *p_fd = (unix_file *)fd;   /* The underlying database file */
#endif
  *pp = 0;

#if DBSQL_MAX_MMAP_SIZE>0
  if( p_fd->mmap_size_max>0 ){
    /* Ensure that there is always at least a 256 byte buffer of addressable
    ** memory following the returned page. If the database is corrupt,
    ** dbsql may overread the page slightly (in practice only a few bytes,
    ** but 256 is safe, round, number).  */
    const int n_eof_buffer = 256;
    if( p_fd->map_region==0 ){
      int rc = __unix_mapfile(p_fd, -1);
      if( rc!=DBSQL_OK ) return rc;
    }
    if( p_fd->mmap_size >= (i_off+n_amt+n_eof_buffer) ){
      *pp = &((u_int8_t *)p_fd->map_region)[i_off];
      p_fd->n_fetch_out++;
    }
  }
#endif
  return DBSQL_OK;
}

/*
** If the third argument is non-NULL, then this function releases a
** reference obtained by an earlier call to __unix_fetch(). The second
** argument passed to this function must be the same as the corresponding
** argument that was passed to the __unix_fetch() invocation.
**
** Or, if the third argument is NULL, then this function is being called
** to inform the VFS layer that, according to POSIX, any existing mapping
** may now be invalid and should be unmapped.
*/
static int __unix_unfetch(dbsql_file_t *fd, int64_t i_off, void *p){
#if DBSQL_MAX_MMAP_SIZE>0
  unix_file *p_fd = (unix_file *)fd;   /* The underlying database file */
  UNUSED_PARAMETER(i_off);

  /* If p==0 (unmap the entire file) then there must be no outstanding
  ** fetch references. Or, if p!=0 (meaning it is an fetch reference),
  ** then there must be at least one outstanding.  */
  assert( (p==0)==(p_fd->n_fetch_out==0) );

  /* If p!=0, it must match the i_off value. */
  assert( p==0 || p==&((u_int8_t *)p_fd->map_region)[i_off] );

  if( p ){
    p_fd->n_fetch_out--;
  }else{
    __unix_unmapfile(p_fd);
  }

  assert( p_fd->n_fetch_out>=0 );
#else
  UNUSED_PARAMETER(fd);
  UNUSED_PARAMETER(p);
  UNUSED_PARAMETER(i_off);
#endif
  return DBSQL_OK;
}

/*
** Here ends the implementation of all dbsql_file_t methods.
**
********************** End dbsql_file_t Methods *******************************
******************************************************************************/

/*
** This division contains definitions of dbsql_io_methods_t objects that
** implement various file locking strategies.  It also contains definitions
** of "finder" functions.  A finder-function is used to locate the appropriate
** dbsql_io_methods_t object for a particular database file.  The app_data
** field of the dbsql_vfs_t VFS objects are initialized to be pointers to
** the correct finder-function for that VFS.
**
** Most finder functions return a pointer to a fixed dbsql_io_methods_t
** object.  The only interesting finder-function is autolock_io_finder, which
** looks at the filesystem type and tries to guess the best locking
** strategy from that.
**
** For finder-function F, two objects are created:
**
**    (1) The real finder-function named "FImpt()".
**
**    (2) A constant pointer to this function named just "F".
**
**
** A pointer to the F pointer is used as the app_data value for VFS
** objects.  We have to do this instead of letting app_data point
** directly at the finder-function since C90 rules prevent a void*
** from be cast into a function pointer.
**
**
** Each instance of this macro generates two objects:
**
**   *  A constant dbsql_io_methods_t object call METHOD that has locking
**      methods CLOSE, LOCK, UNLOCK, CKRESLOCK.
**
**   *  An I/O method finder function called FINDER that returns a pointer
**      to the METHOD object in the previous bullet.
*/
#define IOMETHODS(FINDER,METHOD,VERSION,CLOSE,LOCK,UNLOCK,CKLOCK,SHMMAP)     \
static const dbsql_io_methods_t METHOD = {                                   \
   VERSION,                    /* i_version */                                \
   CLOSE,                      /* x_close */                                  \
   __unix_read,                   /* x_read */                                   \
   __unix_write,                  /* x_write */                                  \
   __unix_truncate,               /* truncate */                               \
   __unix_sync,                   /* x_sync */                                   \
   __unix_file_size,               /* file_size */                               \
   LOCK,                       /* lock */                                   \
   UNLOCK,                     /* unlock */                                 \
   CKLOCK,                     /* check_reserved_lock */                      \
   __unix_file_control,            /* file_control */                            \
   __unix_sector_size,             /* sector_size */                             \
   __unix_device_characteristics,  /* device_capabilities */                     \
   SHMMAP,                     /* shm_map */                                 \
   UNIX_SHM_LOCK,                /* shm_lock */                                \
   UNIX_SHM_BARRIER,             /* shm_barrier */                             \
   UNIX_SHM_UNMAP,               /* shm_unmap */                               \
   __unix_fetch,                  /* fetch */                                  \
   __unix_unfetch,                /* unfetch */                                \
};                                                                           \
static const dbsql_io_methods_t *FINDER##Impl(const char *z, unix_file *p){   \
  UNUSED_PARAMETER(z); UNUSED_PARAMETER(p);                                  \
  return &METHOD;                                                            \
}                                                                            \
static const dbsql_io_methods_t *(*const FINDER)(const char*,unix_file *p)    \
    = FINDER##Impl;

/*
** Here are all of the dbsql_io_methods_t objects for each of the
** locking strategies.  Functions that return pointers to these methods
** are also created.
*/
IOMETHODS(
  posix_io_finder,            /* Finder function name */
  posix_io_methods,           /* dbsql_io_methods_t object name */
  3,                        /* shared memory and mmap are enabled */
  __unix_close,                /* x_close method */
  __unix_lock,                 /* lock method */
  __unix_unlock,               /* unlock method */
  __unix_check_reserved_lock,    /* check_reserved_lock method */
  UNIX_SHM_MAP                /* shm_map method */
)
IOMETHODS(
  nolock_io_finder,           /* Finder function name */
  nolock_io_methods,          /* dbsql_io_methods_t object name */
  3,                        /* shared memory and mmap are enabled */
  __nolock_close,              /* x_close method */
  __nolock_lock,               /* lock method */
  __nolock_unlock,             /* unlock method */
  __nolock_check_reserved_lock,  /* check_reserved_lock method */
  0                         /* shm_map method */
)
IOMETHODS(
  dotlock_io_finder,          /* Finder function name */
  dotlock_io_methods,         /* dbsql_io_methods_t object name */
  1,                        /* shared memory is disabled */
  __dotlock_close,             /* x_close method */
  __dotlock_lock,              /* lock method */
  __dotlock_unlock,            /* unlock method */
  __dotlock_check_reserved_lock, /* check_reserved_lock method */
  0                         /* shm_map method */
)

#if DBSQL_ENABLE_LOCKING_STYLE
IOMETHODS(
  flock_io_finder,            /* Finder function name */
  flock_io_methods,           /* dbsql_io_methods_t object name */
  1,                        /* shared memory is disabled */
  __flock_close,               /* x_close method */
  __flock_lock,                /* lock method */
  __flock_unlock,              /* unlock method */
  __flock_check_reserved_lock,   /* check_reserved_lock method */
  0                         /* shm_map method */
)
#endif

#if OS_VXWORKS
IOMETHODS(
  sem_io_finder,              /* Finder function name */
  sem_io_methods,             /* dbsql_io_methods_t object name */
  1,                        /* shared memory is disabled */
  __sem_x_close,                /* x_close method */
  __sem_x_lock,                 /* lock method */
  __sem_x_unlock,               /* unlock method */
  __sem_x_check_reserved_lock,    /* check_reserved_lock method */
  0                         /* shm_map method */
)
#endif

#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
IOMETHODS(
  afp_io_finder,              /* Finder function name */
  afp_io_methods,             /* dbsql_io_methods_t object name */
  1,                        /* shared memory is disabled */
  __afp_close,                 /* x_close method */
  __afp_lock,                  /* lock method */
  __afp_unlock,                /* unlock method */
  __afp_check_reserved_lock,     /* check_reserved_lock method */
  0                         /* shm_map method */
)
#endif

/*
** The proxy locking method is a "super-method" in the sense that it
** opens secondary file descriptors for the conch and lock files and
** it uses proxy, dot-file, AFP, and flock() locking methods on those
** secondary files.  For this reason, the division that implements
** proxy locking is located much further down in the file.  But we need
** to go ahead and define the dbsql_io_methods_t and finder function
** for proxy locking here.  So we forward declare the I/O methods.
*/
#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
static int __proxy_close(dbsql_file_t*);
static int __proxy_lock(dbsql_file_t*, int);
static int __proxy_unlock(dbsql_file_t*, int);
static int __proxy_check_reserved_lock(dbsql_file_t*, int*);
IOMETHODS(
  proxy_io_finder,            /* Finder function name */
  proxy_io_methods,           /* dbsql_io_methods_t object name */
  1,                        /* shared memory is disabled */
  __proxy_close,               /* x_close method */
  __proxy_lock,                /* lock method */
  __proxy_unlock,              /* unlock method */
  __proxy_check_reserved_lock,   /* check_reserved_lock method */
  0                         /* shm_map method */
)
#endif

/* nfs lockd on OSX 10.3+ doesn't clear write locks when a read lock is set */
#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
IOMETHODS(
  nfs_io_finder,               /* Finder function name */
  nfs_io_methods,              /* dbsql_io_methods_t object name */
  1,                         /* shared memory is disabled */
  __unix_close,                 /* x_close method */
  __unix_lock,                  /* lock method */
  __nfs_unlock,                 /* unlock method */
  __unix_check_reserved_lock,     /* check_reserved_lock method */
  0                          /* shm_map method */
)
#endif

#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
/*
** This "finder" function attempts to determine the best locking strategy
** for the database file "file_path".  It then returns the dbsql_io_methods_t
** object that implements that strategy.
**
** This is for __mac_osx only.
*/
static const dbsql_io_methods_t *__autolock_io_finder_impl(
  const char *file_path,    /* name of the database file */
  unix_file *p_new           /* open file object for the database file */
){
  static const struct mapping_t {
    const char *filesystem;              /* Filesystem type name */
    const dbsql_io_methods_t *methods;   /* Appropriate locking method */
  } maps[] = {
    { "hfs",    &posix_io_methods },
    { "ufs",    &posix_io_methods },
    { "afpfs",  &afp_io_methods },
    { "smbfs",  &afp_io_methods },
    { "webdav", &nolock_io_methods },
    { 0, 0 }
  };
  int i;
  struct statfs fs_info;
  struct flock lock_info;

  if( !file_path ){
    /* If file_path==NULL that means we are dealing with a transient file
    ** that does not need to be locked. */
    return &nolock_io_methods;
  }
  if( statfs(file_path, &fs_info) != -1 ){
    if( fs_info.f_flags & MNT_RDONLY ){
      return &nolock_io_methods;
    }
    for(i=0; maps[i].filesystem; i++){
      if( strcmp(fs_info.f_fstypename, maps[i].filesystem)==0 ){
        return maps[i].methods;
      }
    }
  }

  /* Default case. Handles, amongst others, "nfs".
  ** Test byte-range lock using fcntl(). If the call succeeds,
  ** assume that the file-system supports POSIX style locks.
  */
  lock_info.l_len = 1;
  lock_info.l_start = 0;
  lock_info.l_whence = SEEK_SET;
  lock_info.l_type = F_RDLCK;
  if( __os_fcntl(p_new->h, F_GETLK, &lock_info)!=-1 ) {
    if( strcmp(fs_info.f_fstypename, "nfs")==0 ){
      return &nfs_io_methods;
    } else {
      return &posix_io_methods;
    }
  }else{
    return &dotlock_io_methods;
  }
}
static const dbsql_io_methods_t
  *(*const autolock_io_finder)(const char*,unix_file*) = __autolock_io_finder_impl;

#endif /* defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE */

#if OS_VXWORKS
/*
** This "finder" function for __vx_works checks to see if posix advisory
** locking works.  If it does, then that is what is used.  If it does not
** work, then fallback to named semaphore locking.
*/
static const dbsql_io_methods_t *__vxworks_io_finder_impl(
  const char *file_path,    /* name of the database file */
  unix_file *p_new           /* the open file object */
){
  struct flock lock_info;

  if( !file_path ){
    /* If file_path==NULL that means we are dealing with a transient file
    ** that does not need to be locked. */
    return &nolock_io_methods;
  }

  /* Test if fcntl() is supported and use POSIX style locks.
  ** Otherwise fall back to the named semaphore method.
  */
  lock_info.l_len = 1;
  lock_info.l_start = 0;
  lock_info.l_whence = SEEK_SET;
  lock_info.l_type = F_RDLCK;
  if( __os_fcntl(p_new->h, F_GETLK, &lock_info)!=-1 ) {
    return &posix_io_methods;
  }else{
    return &sem_io_methods;
  }
}
static const dbsql_io_methods_t
  *(*const vxworks_io_finder)(const char*,unix_file*) = __vxworks_io_finder_impl;

#endif /* OS_VXWORKS */

/*
** An abstract type for a pointer to an IO method finder function:
*/
typedef const dbsql_io_methods_t *(*finder_type)(const char*,unix_file*);


/****************************************************************************
**************************** dbsql_vfs_t methods ****************************
**
** This division contains the implementation of methods on the
** dbsql_vfs_t object.
*/

/*
** Initialize the contents of the unix_file structure pointed to by id.
*/
static int __fill_in_unix_file(
  dbsql_vfs_t *vfs,      /* Pointer to vfs object */
  int h,                  /* Open file descriptor of file being opened */
  dbsql_file_t *id,      /* Write to the unix_file structure here */
  const char *filename,  /* Name of the file being opened */
  int ctrl_flags           /* Zero or more UNIXFILE_* values */
){
  const dbsql_io_methods_t *locking_style;
  unix_file *p_new = (unix_file *)id;
  int rc = DBSQL_OK;

  assert( p_new->inode==NULL );

  /* No locking occurs in temporary files */
  assert( filename!=0 || (ctrl_flags & UNIXFILE_NOLOCK)!=0 );

  OSTRACE(("OPEN    %-3d %s\n", h, filename));
  p_new->h = h;
  p_new->vfs = vfs;
  p_new->path = filename;
  p_new->ctrl_flags = (u_int8_t)ctrl_flags;
#if DBSQL_MAX_MMAP_SIZE>0
  p_new->mmap_size_max = __global_config.sz_mmap;
#endif
  if( dbsql_uri_boolean(((ctrl_flags & UNIXFILE_URI) ? filename : 0),
                           "psow", DBSQL_POWERSAFE_OVERWRITE) ){
    p_new->ctrl_flags |= UNIXFILE_PSOW;
  }
  if( strcmp(vfs->name,"unix-excl")==0 ){
    p_new->ctrl_flags |= UNIXFILE_EXCL;
  }

#if OS_VXWORKS
  p_new->id = __vxworks_find_file_id(filename);
  if( p_new->id==0 ){
    ctrl_flags |= UNIXFILE_NOLOCK;
    rc = DBSQL_NOMEM_BKPT;
  }
#endif

  if( ctrl_flags & UNIXFILE_NOLOCK ){
    locking_style = &nolock_io_methods;
  }else{
    locking_style = (**(finder_type*)vfs->app_data)(filename, p_new);
#if DBSQL_ENABLE_LOCKING_STYLE
    /* Cache filename in the locking context (AFP and dotlock override) for
    ** __proxy_lock activation is possible (remote proxy is based on db name)
    ** filename remains valid until file is closed, to support */
    p_new->locking_context = (void*)filename;
#endif
  }

  if( locking_style == &posix_io_methods
#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE
    || locking_style == &nfs_io_methods
#endif
  ){
    __unix_enter_mutex();
    rc = __find_inode_info(p_new, &p_new->inode);
    if( rc!=DBSQL_OK ){
      /* If an error occurred in __find_inode_info(), close the file descriptor
      ** immediately, before releasing the mutex. __find_inode_info() may fail
      ** in two scenarios:
      **
      **   (a) A call to fstat() failed.
      **   (b) A malloc failed.
      **
      ** Scenario (b) may only occur if the process is holding no other
      ** file descriptors open on the same file. If there were other file
      ** descriptors on this file, then no malloc would be required by
      ** __find_inode_info(). If this is the case, it is quite safe to close
      ** handle h - as it is guaranteed that no posix locks will be released
      ** by doing so.
      **
      ** If scenario (a) caused the error then things are not so safe. The
      ** implicit assumption here is that if fstat() fails, things are in
      ** such bad shape that dropping a lock or two doesn't matter much.
      */
      robust_close(p_new, h, __LINE__);
      h = -1;
    }
    __unix_leave_mutex();
  }

#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
  else if( locking_style == &afp_io_methods ){
    /* AFP locking uses the file path so it needs to be included in
    ** the afp_locking_context.
    */
    afp_locking_context *ctx;
    p_new->locking_context = ctx = dbsql_malloc64( sizeof(*ctx) );
    if( ctx==0 ){
      rc = DBSQL_NOMEM_BKPT;
    }else{
      /* NB: filename exists and remains valid until the file is closed
      ** according to requirement F11141.  So we do not need to make a
      ** copy of the filename. */
      ctx->db_path = filename;
      ctx->reserved = 0;
      srandomdev();
      __unix_enter_mutex();
      rc = __find_inode_info(p_new, &p_new->inode);
      if( rc!=DBSQL_OK ){
        dbsql_free(p_new->locking_context);
        robust_close(p_new, h, __LINE__);
        h = -1;
      }
      __unix_leave_mutex();      
    }
  }
#endif

  else if( locking_style == &dotlock_io_methods ){
    /* Dotfile locking uses the file path so it needs to be included in
    ** the dotlock_locking_context
    */
    char *lock_file;
    int n_filename;
    assert( filename!=0 );
    n_filename = (int)strlen(filename) + 6;
    lock_file = (char *)dbsql_malloc64(n_filename);
    if( lock_file==0 ){
      rc = DBSQL_NOMEM_BKPT;
    }else{
      dbsql_snprintf(n_filename, lock_file, "%s" DOTLOCK_SUFFIX, filename);
    }
    p_new->locking_context = lock_file;
  }

#if OS_VXWORKS
  else if( locking_style == &sem_io_methods ){
    /* Named semaphore locking uses the file path so it needs to be
    ** included in the sem_locking_context
    */
    __unix_enter_mutex();
    rc = __find_inode_info(p_new, &p_new->inode);
    if( (rc==DBSQL_OK) && (p_new->inode->sem==NULL) ){
      char *sem_name = p_new->inode->sem_names;
      int n;
      dbsql_snprintf(MAX_PATHNAME, sem_name, "/%s.sem",
                       p_new->id->canonical_name);
      for( n=1; sem_name[n]; n++ )
        if( sem_name[n]=='/' ) sem_name[n] = '_';
      p_new->inode->sem = sem_open(sem_name, O_CREAT, 0666, 1);
      if( p_new->inode->sem == SEM_FAILED ){
        rc = DBSQL_NOMEM_BKPT;
        p_new->inode->sem_names[0] = '\0';
      }
    }
    __unix_leave_mutex();
  }
#endif

  __store_last_errno(p_new, 0);
#if OS_VXWORKS
  if( rc!=DBSQL_OK ){
    if( h>=0 ){
      robust_close(p_new, h, __LINE__);
      h = -1;
    }
    if( p_new->ctrl_flags & UNIXFILE_DELETE ){
      OS_UNLINK(filename);
    }
    if( p_new->id ){
      __vxworks_release_file_id(p_new->id);
      p_new->id = 0;
    }
  }
#endif
  if( rc!=DBSQL_OK ){
    if( h>=0 ) robust_close(p_new, h, __LINE__);
  }else{
    id->methods = locking_style;
    OPEN_COUNTER(+1);
    __verify_db_file(p_new);
  }
  return rc;
}

/*
** Directories to consider for temp files.
*/
static const char *__az_temp_dirs[] = {
  0,
  0,
  "/var/tmp",
  "/usr/tmp",
  "/tmp",
  "."
};

/*
** Initialize first two members of __az_temp_dirs[] array.
*/
static void __unix_temp_file_init(void){
  __az_temp_dirs[0] = getenv("DBSQL_TMPDIR");
  __az_temp_dirs[1] = getenv("TMPDIR");
}

/*
** Return the name of a directory in which to put temporary files.
** If no suitable temporary file directory can be found, return NULL.
*/
static const char *__unix_temp_file_dir(void){
  unsigned int i = 0;
  struct stat buf;
  const char *dir = dbsql_temp_directory;

  while(1){
    if( dir!=0
#if OS_VXWORKS
     && dir[0]=='/'
#endif
     && OS_STAT(dir, &buf)==0
     && S_ISDIR(buf.st_mode)
     && OS_ACCESS(dir, 03)==0
    ){
      return dir;
    }
    if( i>=sizeof(__az_temp_dirs)/sizeof(__az_temp_dirs[0]) ) break;
    dir = __az_temp_dirs[i++];
  }
  return 0;
}

/*
** Create a temporary file name in buf.  buf must be allocated
** by the calling process and must be big enough to hold at least
** vfs->mx_pathname bytes.
*/
static int __unix_get_tempname(int n_buf, char *buf){
  const char *dir;
  int i_limit = 0;
  int rc = DBSQL_OK;

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that dbsql handles this
  ** function failing.
  */
  buf[0] = 0;
  SIMULATE_IO_ERROR( return DBSQL_IOERR );

  dbsql_mutex_enter(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
  dir = __unix_temp_file_dir();
  if( dir==0 ){
    rc = DBSQL_IOERR_GETTEMPPATH;
  }else{
    do{
      u_int64_t r;
      dbsql_randomness(sizeof(r), &r);
      assert( n_buf>2 );
      buf[n_buf-2] = 0;
      dbsql_snprintf(n_buf, buf, "%s/"DBSQL_TEMP_FILE_PREFIX"%llx%c",
                       dir, r, 0);
      if( buf[n_buf-2]!=0 || (i_limit++)>10 ){
        rc = DBSQL_ERROR;
        break;
      }
    }while( OS_ACCESS(buf,0)==0 );
  }
  dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
  return rc;
}

#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
/*
** Routine to transform a unix_file into a proxy-locking unix_file.
** Implementation in the proxy-lock division, but used by __unix_open()
** if DBSQL_PREFER_PROXY_LOCKING is defined.
*/
static int __proxy_transform_unix_file(unix_file*, const char*);
#endif

/*
** Search for an unused file descriptor that was opened on the database
** file (not a journal or super-journal file) identified by pathname
** path with DBSQL_OPEN_XXX flags matching those passed as the second
** argument to this function.
**
** Such a file descriptor may exist if a database connection was closed
** but the associated file descriptor could not be closed because some
** other file descriptor open on the same file is holding a file-lock.
** Refer to comments in the __unix_close() function and the lengthy comment
** describing "Posix Advisory Locking" at the start of this file for
** further details. Also, ticket #4018.
**
** If a suitable file descriptor is found, then it is returned. If no
** such file descriptor is located, -1 is returned.
*/
static unix_unused_fd_t *__find_reusable_fd(const char *path, int flags){
  unix_unused_fd_t *unused = 0;

  /* Do not search for an unused file descriptor on vxworks. Not because
  ** vxworks would not benefit from the change (it might, we're not sure),
  ** but because no way to test it is currently available. It is better
  ** not to risk breaking vxworks support for the sake of such an obscure
  ** feature.  */
#if !OS_VXWORKS
  struct stat s_stat;                   /* Results of stat() call */

  __unix_enter_mutex();

  /* A stat() call may fail for various reasons. If this happens, it is
  ** almost certain that an open() call on the same path will also fail.
  ** For this reason, if an error occurs in the stat() call here, it is
  ** ignored and -1 is returned. The caller will try to open a new file
  ** descriptor on the same path, fail, and return an error to dbsql.
  **
  ** Even if a subsequent open() call does succeed, the consequences of
  ** not searching for a reusable file descriptor are not dire.  */
  if( __inode_list!=0 && 0==OS_STAT(path, &s_stat) ){
    unix_inode_info *inode;

    inode = __inode_list;
    while( inode && (inode->file_id.dev!=s_stat.st_dev
                     || inode->file_id.ino!=(u_int64_t)s_stat.st_ino) ){
       inode = inode->next;
    }
    if( inode ){
      unix_unused_fd_t **pp;
      assert( dbsql_mutex_notheld(inode->lock_mutex) );
      dbsql_mutex_enter(inode->lock_mutex);
      flags &= (DBSQL_OPEN_READONLY|DBSQL_OPEN_READWRITE);
      for(pp=&inode->unused; *pp && (*pp)->flags!=flags; pp=&((*pp)->next));
      unused = *pp;
      if( unused ){
        *pp = unused->next;
      }
      dbsql_mutex_leave(inode->lock_mutex);
    }
  }
  __unix_leave_mutex();
#endif    /* if !OS_VXWORKS */
  return unused;
}

/*
** Find the mode, uid and gid of file file.
*/
static int __get_file_mode(
  const char *file,              /* File name */
  mode_t *mode,                  /* OUT: Permissions of file */
  uid_t *uid,                    /* OUT: uid of file. */
  gid_t *gid                     /* OUT: gid of file. */
){
  struct stat s_stat;              /* Output of stat() on database file */
  int rc = DBSQL_OK;
  if( 0==OS_STAT(file, &s_stat) ){
    *mode = s_stat.st_mode & 0777;
    *uid = s_stat.st_uid;
    *gid = s_stat.st_gid;
  }else{
    rc = DBSQL_IOERR_FSTAT;
  }
  return rc;
}

/*
** This function is called by __unix_open() to determine the unix permissions
** to create new files with. If no error occurs, then DBSQL_OK is returned
** and a value suitable for passing as the third argument to open(2) is
** written to *mode. If an IO error occurs, an dbsql error code is
** returned and the value of *mode is not modified.
**
** In most cases, this routine sets *mode to 0, which will become
** an indication to robust_open() to create the file using
** DBSQL_DEFAULT_FILE_PERMISSIONS adjusted by the umask.
** But if the file being opened is a WAL or regular journal file, then
** this function queries the file-system for the permissions on the
** corresponding database file and sets *mode to this value. Whenever
** possible, WAL and journal files are created using the same permissions
** as the associated database file.
**
** If the DBSQL_ENABLE_8_3_NAMES option is enabled, then the
** original filename is unavailable.  But 8_3_NAMES is only used for
** FAT filesystems and permissions do not matter there, so just use
** the default permissions.  In 8_3_NAMES mode, leave *mode set to zero.
*/
static int __find_create_file_mode(
  const char *path,              /* Path of file (possibly) being created */
  int flags,                      /* Flags passed as 4th argument to x_open() */
  mode_t *mode,                  /* OUT: Permissions to open file with */
  uid_t *uid,                    /* OUT: uid to set on the file */
  gid_t *gid                     /* OUT: gid to set on the file */
){
  int rc = DBSQL_OK;             /* Return Code */
  *mode = 0;
  *uid = 0;
  *gid = 0;
  if( flags & (DBSQL_OPEN_WAL|DBSQL_OPEN_MAIN_JOURNAL) ){
    char db[MAX_PATHNAME+1];     /* Database file path */
    int n_db;                      /* Number of valid bytes in db */

    /* path is a path to a WAL or journal file. The following block derives
    ** the path to the associated database file from path. This block handles
    ** the following naming conventions:
    **
    **   "<path to db>-journal"
    **   "<path to db>-wal"
    **   "<path to db>-journal_nn"
    **   "<path to db>-wal_nn"
    **
    ** where NN is a decimal number. The NN naming schemes are
    ** used by the test_multiplex.c module.
    **
    ** In normal operation, the journal file name will always contain
    ** a '-' character.  However in 8+3 filename mode, or if a corrupt
    ** rollback journal specifies a super-journal with a goofy name, then
    ** the '-' might be missing or the '-' might be the first character in
    ** the filename.  In that case, just return DBSQL_OK with *mode==0.
    */
    n_db = __strlen30(path) - 1;
    while( n_db>0 && path[n_db]!='.' ){
      if( path[n_db]=='-' ){
        memcpy(db, path, n_db);
        db[n_db] = '\0';
        rc = __get_file_mode(db, mode, uid, gid);
        break;
      }
      n_db--;
    }
  }else if( flags & DBSQL_OPEN_DELETEONCLOSE ){
    *mode = 0600;
  }else if( flags & DBSQL_OPEN_URI ){
    /* If this is a main database file and the file was opened using a URI
    ** filename, check for the "modeof" parameter. If present, interpret
    ** its value as a filename and try to copy the mode, uid and gid from
    ** that file.  */
    const char *z = dbsql_uri_parameter(path, "modeof");
    if( z ){
      rc = __get_file_mode(z, mode, uid, gid);
    }
  }
  return rc;
}

/*
** Open the file path.
**
** Previously, the dbsql OS layer used three functions in place of this
** one:
**
**     __dbsql_os_open_read_write();
**     __dbsql_os_open_read_only();
**     __dbsql_os_open_exclusive();
**
** These calls correspond to the following combinations of flags:
**
**     __read_write() ->     (READWRITE | CREATE)
**     __read_only()  ->     (READONLY)
**     __open_exclusive() -> (READWRITE | CREATE | EXCLUSIVE)
**
** The old __open_exclusive() accepted a boolean argument - "del_flag". If
** true, the file was configured to be automatically deleted when the
** file handle closed. To achieve the same effect using this new
** interface, add the DELETEONCLOSE flag to those specified above for
** __open_exclusive().
*/
static int __unix_open(
  dbsql_vfs_t *vfs,           /* The VFS for which this is the x_open method */
  const char *path,           /* Pathname of file to be opened */
  dbsql_file_t *file,         /* The file descriptor to be filled in */
  int flags,                   /* Input flags to control the opening */
  int *out_flags               /* Output flags returned to dbsql core */
){
  unix_file *p = (unix_file *)file;
  int fd = -1;                   /* File descriptor returned by open() */
  int open_flags = 0;             /* Flags to pass to open() */
  int type = flags&0x0FFF00;  /* Type of file to open */
  int no_lock;                    /* True to omit locking primitives */
  int rc = DBSQL_OK;            /* Function Return Code */
  int ctrl_flags = 0;             /* UNIXFILE_* flags */

  int is_exclusive  = (flags & DBSQL_OPEN_EXCLUSIVE);
  int is_delete     = (flags & DBSQL_OPEN_DELETEONCLOSE);
  int is_create     = (flags & DBSQL_OPEN_CREATE);
  int is_readonly   = (flags & DBSQL_OPEN_READONLY);
  int is_read_write  = (flags & DBSQL_OPEN_READWRITE);
#if DBSQL_ENABLE_LOCKING_STYLE
  int is_auto_proxy  = (flags & DBSQL_OPEN_AUTOPROXY);
#endif
#if defined(__APPLE__) || DBSQL_ENABLE_LOCKING_STYLE
  struct statfs fs_info;
#endif

  /* If creating a super- or main-file journal, this function will open
  ** a file-descriptor on the directory too. The first time __unix_sync()
  ** is called the directory file descriptor will be fsync()ed and close()d.
  */
  int is_new_jrnl = (is_create && (
        type==DBSQL_OPEN_SUPER_JOURNAL
     || type==DBSQL_OPEN_MAIN_JOURNAL
     || type==DBSQL_OPEN_WAL
  ));

  /* If argument path is a NULL pointer, this function is required to open
  ** a temporary file. Use this buffer to store the file name in.
  */
  char tmpname[MAX_PATHNAME+2];
  const char *name = path;

  /* Check the following statements are true:
  **
  **   (a) Exactly one of the READWRITE and READONLY flags must be set, and
  **   (b) if CREATE is set, then READWRITE must also be set, and
  **   (c) if EXCLUSIVE is set, then CREATE must also be set.
  **   (d) if DELETEONCLOSE is set, then CREATE must also be set.
  */
  assert((is_readonly==0 || is_read_write==0) && (is_read_write || is_readonly));
  assert(is_create==0 || is_read_write);
  assert(is_exclusive==0 || is_create);
  assert(is_delete==0 || is_create);

  /* The main DB, main journal, WAL file and super-journal are never
  ** automatically deleted. Nor are they ever temporary files.  */
  assert( (!is_delete && name) || type!=DBSQL_OPEN_MAIN_DB );
  assert( (!is_delete && name) || type!=DBSQL_OPEN_MAIN_JOURNAL );
  assert( (!is_delete && name) || type!=DBSQL_OPEN_SUPER_JOURNAL );
  assert( (!is_delete && name) || type!=DBSQL_OPEN_WAL );

  /* Assert that the upper layer has set one of the "file-type" flags. */
  assert( type==DBSQL_OPEN_MAIN_DB      || type==DBSQL_OPEN_TEMP_DB
       || type==DBSQL_OPEN_MAIN_JOURNAL || type==DBSQL_OPEN_TEMP_JOURNAL
       || type==DBSQL_OPEN_SUBJOURNAL   || type==DBSQL_OPEN_SUPER_JOURNAL
       || type==DBSQL_OPEN_TRANSIENT_DB || type==DBSQL_OPEN_WAL
  );

#if OS_VXWORKS
  /* The file-ID mechanism used in Vxworks requires that all pathnames
  ** provided to __unix_open must be absolute pathnames. */
  if( path!=0 && path[0]!='/' ){ return DBSQL_CANTOPEN; }
#endif

  /* Detect a pid change and reset the PRNG.  There is a race condition
  ** here such that two or more threads all trying to open databases at
  ** the same instant might all reset the PRNG.  But multiple resets
  ** are harmless.
  */
  if( __randomness_pid!=OS_GETPID(0) ){
    __randomness_pid = OS_GETPID(0);
    dbsql_randomness(0,0);
  }
  memset(p, 0, sizeof(unix_file));

#ifdef DBSQL_ASSERT_NO_FILES
  /* Applications that never read or write a persistent disk files */
  assert( name==0 );
#endif

  if( type==DBSQL_OPEN_MAIN_DB ){
    unix_unused_fd_t *unused;
    unused = __find_reusable_fd(name, flags);
    if( unused ){
      fd = unused->fd;
    }else{
      unused = dbsql_malloc64(sizeof(*unused));
      if( !unused ){
        return DBSQL_NOMEM_BKPT;
      }
    }
    p->preallocated_unused = unused;

    /* Database filenames are double-zero terminated if they are not
    ** URIs with parameters.  Hence, they can always be passed into
    ** dbsql_uri_parameter(). */
    assert( (flags & DBSQL_OPEN_URI) || name[strlen(name)+1]==0 );

  }else if( !name ){
    /* If name is NULL, the upper layer is requesting a temp file. */
    assert(is_delete && !is_new_jrnl);
    rc = __unix_get_tempname(vfs->mx_pathname, tmpname);
    if( rc!=DBSQL_OK ){
      return rc;
    }
    name = tmpname;

    /* Generated temporary filenames are always double-zero terminated
    ** for use by dbsql_uri_parameter(). */
    assert( name[strlen(name)+1]==0 );
  }

  /* Determine the value of the flags parameter passed to POSIX function
  ** open(). These must be calculated even if open() is not called, as
  ** they may be stored as part of the file handle and used by the
  ** 'conch file' locking functions later on.  */
  if( is_readonly )  open_flags |= O_RDONLY;
  if( is_read_write ) open_flags |= O_RDWR;
  if( is_create )    open_flags |= O_CREAT;
  if( is_exclusive ) open_flags |= (O_EXCL|O_NOFOLLOW);
  open_flags |= (O_LARGEFILE|O_BINARY|O_NOFOLLOW);

  if( fd<0 ){
    mode_t open_mode;              /* Permissions to create file with */
    uid_t uid;                    /* Userid for the file */
    gid_t gid;                    /* Groupid for the file */
    rc = __find_create_file_mode(name, flags, &open_mode, &uid, &gid);
    if( rc!=DBSQL_OK ){
      assert( !p->preallocated_unused );
      assert( type==DBSQL_OPEN_WAL || type==DBSQL_OPEN_MAIN_JOURNAL );
      return rc;
    }
    fd = robust_open(name, open_flags, open_mode);
    OSTRACE(("OPENX   %-3d %s 0%o\n", fd, name, open_flags));
    assert( !is_exclusive || (open_flags & O_CREAT)!=0 );
    if( fd<0 ){
      if( is_new_jrnl && errno==EACCES && OS_ACCESS(name, F_OK) ){
        /* If unable to create a journal because the directory is not
        ** writable, change the error code to indicate that. */
        rc = DBSQL_READONLY_DIRECTORY;
      }else if( errno!=EISDIR && is_read_write ){
        /* Failed to open the file for read/write access. Try read-only. */
        unix_unused_fd_t *readonly = 0;
        flags &= ~(DBSQL_OPEN_READWRITE|DBSQL_OPEN_CREATE);
        open_flags &= ~(O_RDWR|O_CREAT);
        flags |= DBSQL_OPEN_READONLY;
        open_flags |= O_RDONLY;
        is_readonly = 1;
        readonly = __find_reusable_fd(name, flags);
        if( readonly ){
          fd = readonly->fd;
          dbsql_free(readonly);
        }else{
          fd = robust_open(name, open_flags, open_mode);
        }
      }
    }
    if( fd<0 ){
      int rc2 = UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "open", name);
      if( rc==DBSQL_OK ) rc = rc2;
      goto open_finished;
    }

    /* The owner of the rollback journal or WAL file should always be the
    ** same as the owner of the database file.  Try to ensure that this is
    ** the case.  The chown() system call will be a no-op if the current
    ** process lacks root privileges, be we should at least try.  Without
    ** this step, if a root process opens a database file, it can leave
    ** behinds a journal/WAL that is owned by root and hence make the
    ** database inaccessible to unprivileged processes.
    **
    ** If open_mode==0, then that means uid and gid are not set correctly
    ** (probably because dbsql is configured to use 8+3 filename mode) and
    ** in that case we do not want to attempt the chown().
    */
    if( open_mode && (flags & (DBSQL_OPEN_WAL|DBSQL_OPEN_MAIN_JOURNAL))!=0 ){
      __robust_fchown(fd, uid, gid);
    }
  }
  assert( fd>=0 );
  if( out_flags ){
    *out_flags = flags;
  }

  if( p->preallocated_unused ){
    p->preallocated_unused->fd = fd;
    p->preallocated_unused->flags =
                          flags & (DBSQL_OPEN_READONLY|DBSQL_OPEN_READWRITE);
  }

  if( is_delete ){
#if OS_VXWORKS
    path = name;
#elif defined(DBSQL_UNLINK_AFTER_CLOSE)
    path = dbsql_mprintf("%s", name);
    if( path==0 ){
      robust_close(p, fd, __LINE__);
      return DBSQL_NOMEM_BKPT;
    }
#else
    OS_UNLINK(name);
#endif
  }
#if DBSQL_ENABLE_LOCKING_STYLE
  else{
    p->open_flags = open_flags;
  }
#endif

#if defined(__APPLE__) || DBSQL_ENABLE_LOCKING_STYLE
  if( fstatfs(fd, &fs_info) == -1 ){
    __store_last_errno(p, errno);
    robust_close(p, fd, __LINE__);
    return DBSQL_IOERR_ACCESS;
  }
  if (0 == strncmp("msdos", fs_info.f_fstypename, 5)) {
    ((unix_file*)file)->fs_flags |= DBSQL_FSFLAGS_IS_MSDOS;
  }
  if (0 == strncmp("exfat", fs_info.f_fstypename, 5)) {
    ((unix_file*)file)->fs_flags |= DBSQL_FSFLAGS_IS_MSDOS;
  }
#endif

  /* Set up appropriate ctrl_flags */
  if( is_delete )                ctrl_flags |= UNIXFILE_DELETE;
  if( is_readonly )              ctrl_flags |= UNIXFILE_RDONLY;
  no_lock = type!=DBSQL_OPEN_MAIN_DB;
  if( no_lock )                  ctrl_flags |= UNIXFILE_NOLOCK;
  if( is_new_jrnl )               ctrl_flags |= UNIXFILE_DIRSYNC;
  if( flags & DBSQL_OPEN_URI ) ctrl_flags |= UNIXFILE_URI;

#if DBSQL_ENABLE_LOCKING_STYLE
#if DBSQL_PREFER_PROXY_LOCKING
  is_auto_proxy = 1;
#endif
  if( is_auto_proxy && (path!=NULL) && (!no_lock) && vfs->x_open ){
    char *envforce = getenv("DBSQL_FORCE_PROXY_LOCKING");
    int use_proxy = 0;

    /* DBSQL_FORCE_PROXY_LOCKING==1 means force always use proxy, 0 means
    ** never use proxy, NULL means use proxy for non-local files only.  */
    if( envforce!=NULL ){
      use_proxy = atoi(envforce)>0;
    }else{
      use_proxy = !(fs_info.f_flags&MNT_LOCAL);
    }
    if( use_proxy ){
      rc = __fill_in_unix_file(vfs, fd, file, path, ctrl_flags);
      if( rc==DBSQL_OK ){
        rc = __proxy_transform_unix_file((unix_file*)file, ":auto:");
        if( rc!=DBSQL_OK ){
          /* Use __unix_close to clean up the resources added in __fill_in_unix_file
          ** and clear all the structure's references.  Specifically,
          ** file->methods will be NULL so __dbsql_os_close will be a no-op
          */
          __unix_close(file);
          return rc;
        }
      }
      goto open_finished;
    }
  }
#endif

  assert( path==0
       || path[0]=='/'
       || type==DBSQL_OPEN_SUPER_JOURNAL
       || type==DBSQL_OPEN_MAIN_JOURNAL
       || type==DBSQL_OPEN_TEMP_JOURNAL
  );
  rc = __fill_in_unix_file(vfs, fd, file, path, ctrl_flags);

open_finished:
  if( rc!=DBSQL_OK ){
    dbsql_free(p->preallocated_unused);
  }
  return rc;
}


/*
** Delete the file at path. If the dir_sync argument is true, fsync()
** the directory after deleting the file.
*/
static int __unix_delete(
  dbsql_vfs_t *__not_used,     /* VFS containing this as the x_delete method */
  const char *path,        /* Name of file to be deleted */
  int dir_sync               /* If true, fsync() directory after deleting file */
){
  int rc = DBSQL_OK;
  UNUSED_PARAMETER(__not_used);
  SIMULATE_IO_ERROR(return DBSQL_IOERR_DELETE);
  if( OS_UNLINK(path)==(-1) ){
    if( errno==ENOENT
#if OS_VXWORKS
        || OS_ACCESS(path,0)!=0
#endif
    ){
      rc = DBSQL_IOERR_DELETE_NOENT;
    }else{
      rc = UNIX_LOG_ERROR(DBSQL_IOERR_DELETE, "unlink", path);
    }
    return rc;
  }
#ifndef DBSQL_DISABLE_DIRSYNC
  if( (dir_sync & 1)!=0 ){
    int fd;
    rc = OS_OPEN_DIRECTORY(path, &fd);
    if( rc==DBSQL_OK ){
      if( full_fsync(fd,0,0) ){
        rc = UNIX_LOG_ERROR(DBSQL_IOERR_DIR_FSYNC, "fsync", path);
      }
      robust_close(0, fd, __LINE__);
    }else{
      assert( rc==DBSQL_CANTOPEN );
      rc = DBSQL_OK;
    }
  }
#endif
  return rc;
}

/*
** Test the existence of or access permissions of file path. The
** test performed depends on the value of flags:
**
**     DBSQL_ACCESS_EXISTS: Return 1 if the file exists
**     DBSQL_ACCESS_READWRITE: Return 1 if the file is read and writable.
**     DBSQL_ACCESS_READONLY: Return 1 if the file is readable.
**
** Otherwise return 0.
*/
static int __unix_access(
  dbsql_vfs_t *__not_used,   /* The VFS containing this x_access method */
  const char *path,      /* Path of the file to examine */
  int flags,              /* What do we want to learn about the path file? */
  int *res_out            /* Write result boolean here */
){
  UNUSED_PARAMETER(__not_used);
  SIMULATE_IO_ERROR( return DBSQL_IOERR_ACCESS; );
  assert( res_out!=0 );

  /* The spec says there are three possible values for flags.  But only
  ** two of them are actually used */
  assert( flags==DBSQL_ACCESS_EXISTS || flags==DBSQL_ACCESS_READWRITE );

  if( flags==DBSQL_ACCESS_EXISTS ){
    struct stat buf;
    *res_out = 0==OS_STAT(path, &buf) &&
                (!S_ISREG(buf.st_mode) || buf.st_size>0);
  }else{
    *res_out = OS_ACCESS(path, W_OK|R_OK)==0;
  }
  return DBSQL_OK;
}

/*
** A pathname under construction
*/
typedef struct db_path_t db_path_t;
struct db_path_t {
  int rc;           /* Non-zero following any error */
  int n_symlink;     /* Number of symlinks resolved */
  char *out;       /* Write the pathname here */
  int n_out;         /* Bytes of space available to out[] */
  int n_used;        /* Bytes of out[] currently being used */
};

/* Forward reference */
static void __append_all_path_elements(db_path_t*,const char*);

/*
** Append a single path element to the db_path_t under construction
*/
static void __append_one_path_element(
  db_path_t *path,       /* Path under construction, to which to append name */
  const char *name,   /* Name to append to path.  Not zero-terminated */
  int n_name            /* Number of significant bytes in name */
){
  assert( n_name>0 );
  assert( name!=0 );
  if( name[0]=='.' ){
    if( n_name==1 ) return;
    if( name[1]=='.' && n_name==2 ){
      if( path->n_used>1 ){
        assert( path->out[0]=='/' );
        while( path->out[--path->n_used]!='/' ){}
      }
      return;
    }
  }
  if( path->n_used + n_name + 2 >= path->n_out ){
    path->rc = DBSQL_ERROR;
    return;
  }
  path->out[path->n_used++] = '/';
  memcpy(&path->out[path->n_used], name, n_name);
  path->n_used += n_name;
#if defined(HAVE_READLINK) && defined(HAVE_LSTAT)
  if( path->rc==DBSQL_OK ){
    const char *in;
    struct stat buf;
    path->out[path->n_used] = 0;
    in = path->out;
    if( OS_LSTAT(in, &buf)!=0 ){
      if( errno!=ENOENT ){
        path->rc = UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "lstat", in);
      }
    }else if( S_ISLNK(buf.st_mode) ){
      ssize_t got;
      char lnk[DBSQL_MAX_PATHLEN+2];
      if( path->n_symlink++ > DBSQL_MAX_SYMLINK ){
        path->rc = DBSQL_CANTOPEN_BKPT;
        return;
      }
      got = OS_READLINK(in, lnk, sizeof(lnk)-2);
      if( got<=0 || got>=(ssize_t)sizeof(lnk)-2 ){
        path->rc = UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "readlink", in);
        return;
      }
      lnk[got] = 0;
      if( lnk[0]=='/' ){
        path->n_used = 0;
      }else{
        path->n_used -= n_name + 1;
      }
      __append_all_path_elements(path, lnk);
    }
  }
#endif
}

/*
** Append all path elements in path to the db_path_t under construction.
*/
static void __append_all_path_elements(
  db_path_t *path,       /* Path under construction, to which to append name */
  const char *z_path    /* Path to append to path.  Is zero-terminated */
){
  int i = 0;
  int j = 0;
  do{
    while( z_path[i] && z_path[i]!='/' ){ i++; }
    if( i>j ){
      __append_one_path_element(path, &z_path[j], i-j);
    }
    j = i+1;
  }while( z_path[i++] );
}

/*
** Turn a relative pathname into a full pathname. The relative path
** is stored as a nul-terminated string in the buffer pointed to by
** path.
**
** out points to a buffer of at least dbsql_vfs_t.mx_pathname bytes
** (in this case, MAX_PATHNAME bytes). The full-path is written to
** this buffer before returning.
*/
static int __unix_full_pathname(
  dbsql_vfs_t *vfs,            /* Pointer to vfs object */
  const char *z_path,            /* Possibly relative input path */
  int n_out,                     /* Size of output buffer in bytes */
  char *out                    /* Output buffer */
){
  db_path_t path;
  UNUSED_PARAMETER(vfs);
  path.rc = 0;
  path.n_used = 0;
  path.n_symlink = 0;
  path.n_out = n_out;
  path.out = out;
  if( z_path[0]!='/' ){
    char pwd[DBSQL_MAX_PATHLEN+2];
    if( OS_GETCWD(pwd, sizeof(pwd)-2)==0 ){
      return UNIX_LOG_ERROR(DBSQL_CANTOPEN_BKPT, "getcwd", z_path);
    }
    __append_all_path_elements(&path, pwd);
  }
  __append_all_path_elements(&path, z_path);
  out[path.n_used] = 0;
  if( path.rc || path.n_used<2 ) return DBSQL_CANTOPEN_BKPT;
  if( path.n_symlink ) return DBSQL_OK_SYMLINK;
  return DBSQL_OK;
}

#ifndef DBSQL_OMIT_LOAD_EXTENSION
/*
** Interfaces for opening a shared library, finding entry points
** within the shared library, and closing the shared library.
*/
#include <dlfcn.h>
static void *UNIX_DL_OPEN(dbsql_vfs_t *__not_used, const char *filename){
  UNUSED_PARAMETER(__not_used);
  return dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
}

/*
** dbsql calls this function immediately after a call to UNIX_DL_SYM() or
** UNIX_DL_OPEN() fails (returns a null pointer). If a more detailed error
** message is available, it is written to buf_out. If no error message
** is available, buf_out is left unmodified and dbsql uses a default
** error message.
*/
static void UNIX_DL_ERROR(dbsql_vfs_t *__not_used, int n_buf, char *buf_out){
  const char *err;
  UNUSED_PARAMETER(__not_used);
  __unix_enter_mutex();
  err = dlerror();
  if( err ){
    dbsql_snprintf(n_buf, buf_out, "%s", err);
  }
  __unix_leave_mutex();
}
static void (*UNIX_DL_SYM(dbsql_vfs_t *__not_used, void *p, const char*sym))(void){
  /*
  ** GCC with -pedantic-errors says that C90 does not allow a void* to be
  ** cast into a pointer to a function.  And yet the library dlsym() routine
  ** returns a void* which is really a pointer to a function.  So how do we
  ** use dlsym() with -pedantic-errors?
  **
  ** Variable x below is defined to be a pointer to a function taking
  ** parameters void* and const char* and returning a pointer to a function.
  ** We initialize x by assigning it a pointer to the dlsym() function.
  ** (That assignment requires a cast.)  Then we call the function that
  ** x points to.
  **
  ** This work-around is unlikely to work correctly on any system where
  ** you really cannot cast a function pointer into void*.  But then, on the
  ** other hand, dlsym() will not work on such a system either, so we have
  ** not really lost anything.
  */
  void (*(*x)(void*,const char*))(void);
  UNUSED_PARAMETER(__not_used);
  x = (void(*(*)(void*,const char*))(void))dlsym;
  return (*x)(p, sym);
}
static void UNIX_DL_CLOSE(dbsql_vfs_t *__not_used, void *handle){
  UNUSED_PARAMETER(__not_used);
  dlclose(handle);
}
#else /* if DBSQL_OMIT_LOAD_EXTENSION is defined: */
  #define UNIX_DL_OPEN  0
  #define UNIX_DL_ERROR 0
  #define UNIX_DL_SYM   0
  #define UNIX_DL_CLOSE 0
#endif

/*
** Write n_buf bytes of random data to the supplied buffer buf.
*/
static int __unix_randomness(dbsql_vfs_t *__not_used, int n_buf, char *buf){
  UNUSED_PARAMETER(__not_used);
  assert((size_t)n_buf>=(sizeof(time_t)+sizeof(int)));

  /* We have to initialize buf to prevent valgrind from reporting
  ** errors.  The reports issued by valgrind are incorrect - we would
  ** prefer that the randomness be increased by making use of the
  ** uninitialized space in buf - but valgrind errors tend to worry
  ** some users.  Rather than argue, it seems easier just to initialize
  ** the whole array and silence valgrind, even if that means less randomness
  ** in the random seed.
  **
  ** When testing, initializing buf[] to zero is all we do.  That means
  ** that we always use the same random number sequence.  This makes the
  ** tests repeatable.
  */
  memset(buf, 0, n_buf);
  __randomness_pid = OS_GETPID(0);
#if !defined(DBSQL_TEST) && !defined(DBSQL_OMIT_RANDOMNESS)
  {
    int fd, got;
    fd = robust_open("/dev/urandom", O_RDONLY, 0);
    if( fd<0 ){
      time_t t;
      time(&t);
      memcpy(buf, &t, sizeof(t));
      memcpy(&buf[sizeof(t)], &__randomness_pid, sizeof(__randomness_pid));
      assert( sizeof(t)+sizeof(__randomness_pid)<=(size_t)n_buf );
      n_buf = sizeof(t) + sizeof(__randomness_pid);
    }else{
      do{ got = OS_READ(fd, buf, n_buf); }while( got<0 && errno==EINTR );
      robust_close(0, fd, __LINE__);
    }
  }
#endif
  return n_buf;
}


/*
** Sleep for a little while.  Return the amount of time slept.
** The argument is the number of microseconds we want to sleep.
** The return value is the number of microseconds of sleep actually
** requested from the underlying operating system, a number which
** might be greater than or equal to the argument, but not less
** than the argument.
*/
static int __unix_sleep(dbsql_vfs_t *__not_used, int microseconds){
#if !defined(HAVE_NANOSLEEP) || HAVE_NANOSLEEP+0
  struct timespec sp;
  sp.tv_sec = microseconds / 1000000;
  sp.tv_nsec = (microseconds % 1000000) * 1000;

  /* Almost all modern unix systems support nanosleep().  But if you are
  ** compiling for one of the rare exceptions, you can use
  ** -DHAVE_NANOSLEEP=0 (perhaps in conjunction with -DHAVE_USLEEP if
  ** usleep() is available) in order to bypass the use of nanosleep() */
  nanosleep(&sp, NULL);

  UNUSED_PARAMETER(__not_used);
  return microseconds;
#elif defined(HAVE_USLEEP) && HAVE_USLEEP
  if( microseconds>=1000000 ) sleep(microseconds/1000000);
  if( microseconds%1000000 ) usleep(microseconds%1000000);
  UNUSED_PARAMETER(__not_used);
  return microseconds;
#else
  int seconds = (microseconds+999999)/1000000;
  sleep(seconds);
  UNUSED_PARAMETER(__not_used);
  return seconds*1000000;
#endif
}

/*
** The following variable, if set to a non-zero value, is interpreted as
** the number of seconds since 1970 and is used to set the result of
** __dbsql_os_current_time() during testing.
*/
#ifdef DBSQL_TEST
int dbsql_current_time = 0;  /* Fake system time in seconds since 1970. */
#endif

/*
** Find the current time (in Universal Coordinated Time).  Write into *pi_now
** the current time and date as a Julian Day number times 86_400_000.  In
** other words, write into *pi_now the number of milliseconds since the Julian
** epoch of noon in Greenwich on November 24, 4714 B.C according to the
** proleptic Gregorian calendar.
**
** On success, return DBSQL_OK.  Return DBSQL_ERROR if the time and date
** cannot be found.
*/
static int __unix_current_time_int64(dbsql_vfs_t *__not_used, int64_t *pi_now){
  static const int64_t unix_epoch = 24405875*(int64_t)8640000;
  int rc = DBSQL_OK;
#if defined(NO_GETTOD)
  time_t t;
  time(&t);
  *pi_now = ((int64_t)t)*1000 + unix_epoch;
#elif OS_VXWORKS
  struct timespec s_now;
  clock_gettime(CLOCK_REALTIME, &s_now);
  *pi_now = unix_epoch + 1000*(int64_t)s_now.tv_sec + s_now.tv_nsec/1000000;
#else
  struct timeval s_now;
  (void)gettimeofday(&s_now, 0);  /* Cannot fail given valid arguments */
  *pi_now = unix_epoch + 1000*(int64_t)s_now.tv_sec + s_now.tv_usec/1000;
#endif

#ifdef DBSQL_TEST
  if( dbsql_current_time ){
    *pi_now = 1000*(int64_t)dbsql_current_time + unix_epoch;
  }
#endif
  UNUSED_PARAMETER(__not_used);
  return rc;
}

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *pr_now and
** return 0.  Return 1 if the time and date cannot be found.
*/
static int UNIX_CURRENT_TIME(dbsql_vfs_t *__not_used, double *pr_now){
  int64_t i = 0;
  int rc;
  UNUSED_PARAMETER(__not_used);
  rc = __unix_current_time_int64(0, &i);
  *pr_now = i/86400000.0;
  return rc;
}
#else
# define UNIX_CURRENT_TIME 0
#endif

/*
** The get_last_error() method is designed to return a better
** low-level error message when operating-system problems come up
** during dbsql operation.  Only the integer return code is currently
** used.
*/
static int __unix_get_last_error(dbsql_vfs_t *__not_used, int __not_used2, char *__not_used3){
  UNUSED_PARAMETER(__not_used);
  UNUSED_PARAMETER(__not_used2);
  UNUSED_PARAMETER(__not_used3);
  return errno;
}


/*
************************ End of dbsql_vfs_t methods ***************************
******************************************************************************/

/******************************************************************************
************************** Begin Proxy Locking ********************************
**
** Proxy locking is a "uber-locking-method" in this sense:  It uses the
** other locking methods on secondary lock files.  Proxy locking is a
** meta-layer over top of the primitive locking implemented above.  For
** this reason, the division that implements of proxy locking is deferred
** until late in the file (here) after all of the other I/O methods have
** been defined - so that the primitive locking methods are available
** as services to help with the implementation of proxy locking.
**
****
**
** The default locking schemes in dbsql use byte-range locks on the
** database file to coordinate safe, concurrent access by multiple readers
** and writers [http://sqlite.org/lockingv3.html].  The five file locking
** states (UNLOCKED, PENDING, SHARED, RESERVED, EXCLUSIVE) are implemented
** as POSIX read & write locks over fixed set of locations (via fsctl),
** on AFP and SMB only exclusive byte-range locks are available via fsctl
** with _IOWR('z', 23, struct byte_range_lock_pb2_t) to track the same 5 states.
** To simulate a F_RDLCK on the shared range, on AFP a randomly selected
** address in the shared range is taken for a SHARED lock, the entire
** shared range is taken for an EXCLUSIVE lock):
**
**      PENDING_BYTE        0x40000000
**      RESERVED_BYTE       0x40000001
**      SHARED_RANGE        0x40000002 -> 0x40000200
**
** This works well on the local file system, but shows a nearly 100x
** slowdown in read performance on AFP because the AFP client disables
** the read cache when byte-range locks are present.  Enabling the read
** cache exposes a cache coherency problem that is present on all OS X
** supported network file systems.  NFS and AFP both observe the
** close-to-open semantics for ensuring cache coherency
** [http://nfs.sourceforge.net/#faq_a8], which does not effectively
** address the requirements for concurrent database access by multiple
** readers and writers
** [http://www.nabble.com/dbsql-on-NFS-cache-coherency-td15655701.html].
**
** To address the performance and cache coherency issues, proxy file locking
** changes the way database access is controlled by limiting access to a
** single host at a time and moving file locks off of the database file
** and onto a proxy file on the local file system.
**
**
** Using proxy locks
** -----------------
**
** C APIs
**
**  dbsql_file_control(db, dbname, DBSQL_FCNTL_SET_LOCKPROXYFILE,
**                       <proxy_path> | ":auto:");
**  dbsql_file_control(db, dbname, DBSQL_FCNTL_GET_LOCKPROXYFILE,
**                       &<proxy_path>);
**
**
** SQL pragmas
**
**  PRAGMA [database.]lock_proxy_file=<proxy_path> | :auto:
**  PRAGMA [database.]lock_proxy_file
**
** Specifying ":auto:" means that if there is a conch file with a matching
** host ID in it, the proxy path in the conch file will be used, otherwise
** a proxy path based on the user's temp dir
** (via confstr(_CS_DARWIN_USER_TEMP_DIR,...)) will be used and the
** actual proxy file name is generated from the name and path of the
** database file.  For example:
**
**       For database path "/Users/me/foo.db"
**       The lock path will be "<tmpdir>/sqliteplocks/_Users_me_foo.db:auto:")
**
** Once a lock proxy is configured for a database connection, it can not
** be removed, however it may be switched to a different proxy path via
** the above APIs (assuming the conch file is not being held by another
** connection or process).
**
**
** How proxy locking works
** -----------------------
**
** Proxy file locking relies primarily on two new supporting files:
**
**   *  conch file to limit access to the database file to a single host
**      at a time
**
**   *  proxy file to act as a proxy for the advisory locks normally
**      taken on the database
**
** The conch file - to use a proxy file, sqlite must first "hold the conch"
** by taking an sqlite-style shared lock on the conch file, reading the
** contents and comparing the host's unique host ID (see below) and lock
** proxy path against the values stored in the conch.  The conch file is
** stored in the same directory as the database file and the file name
** is patterned after the database file name as ".<databasename>-conch".
** If the conch file does not exist, or its contents do not match the
** host ID and/or proxy path, then the lock is escalated to an exclusive
** lock and the conch file contents is updated with the host ID and proxy
** path and the lock is downgraded to a shared lock again.  If the conch
** is held by another process (with a shared lock), the exclusive lock
** will fail and DBSQL_BUSY is returned.
**
** The proxy file - a single-byte file used for all advisory file locks
** normally taken on the database file.   This allows for safe sharing
** of the database file for multiple readers and writers on the same
** host (the conch ensures that they all use the same local lock file).
**
** Requesting the lock proxy does not immediately take the conch, it is
** only taken when the first request to lock database file is made.
** This matches the semantics of the traditional locking behavior, where
** opening a connection to a database file does not take a lock on it.
** The shared lock and an open file descriptor are maintained until
** the connection to the database is closed.
**
** The proxy file and the lock file are never deleted so they only need
** to be created the first time they are used.
**
** Configuration options
** ---------------------
**
**  DBSQL_PREFER_PROXY_LOCKING
**
**       Database files accessed on non-local file systems are
**       automatically configured for proxy locking, lock files are
**       named automatically using the same logic as
**       PRAGMA lock_proxy_file=":auto:"
**  
**  DBSQL_PROXY_DEBUG
**
**       Enables the logging of error messages during host id file
**       retrieval and creation
**
**  LOCKPROXYDIR
**
**       Overrides the default directory used for lock proxy files that
**       are named automatically via the ":auto:" setting
**
**  DBSQL_DEFAULT_PROXYDIR_PERMISSIONS
**
**       Permissions to use when creating a directory for storing the
**       lock proxy files, only used when LOCKPROXYDIR is not set.
**  
**  
** As mentioned above, when compiled with DBSQL_PREFER_PROXY_LOCKING,
** setting the environment variable DBSQL_FORCE_PROXY_LOCKING to 1 will
** force proxy locking to be used for every database file opened, and 0
** will force automatic proxy locking to be disabled for all database
** files (explicitly calling the DBSQL_FCNTL_SET_LOCKPROXYFILE pragma or
** dbsql_file_control API is not affected by DBSQL_FORCE_PROXY_LOCKING).
*/

/*
** Proxy locking is only available on __mac_osx
*/
#if defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE

/*
** The proxy_locking_context has the path and file structures for the remote
** and local proxy files in it
*/
typedef struct proxy_locking_context proxy_locking_context;
struct proxy_locking_context {
  unix_file *conch_file;         /* Open conch file */
  char *conch_file_path;         /* Name of the conch file */
  unix_file *lock_proxy;         /* Open proxy lock file */
  char *lock_proxy_path;         /* Name of the proxy lock file */
  char *db_path;                /* Name of the open file */
  int conch_held;               /* 1 if the conch is held, -1 if lockless */
  int n_fails;                  /* Number of conch taking failures */
  void *old_locking_context;     /* Original lockingcontext to restore on close */
  dbsql_io_methods_t const *old_method;     /* Original I/O methods for close */
};

/*
** The proxy lock file path for the database at db_path is written into l_path,
** which must point to valid, writable memory large enough for a max_len length
** file path.
*/
static int __proxy_get_lock_path(const char *db_path, char *l_path, size_t max_len){
  int len;
  int db_len;
  int i;

#ifdef LOCKPROXYDIR
  len = strlcpy(l_path, LOCKPROXYDIR, max_len);
#else
# ifdef _CS_DARWIN_USER_TEMP_DIR
  {
    if( !confstr(_CS_DARWIN_USER_TEMP_DIR, l_path, max_len) ){
      OSTRACE(("GETLOCKPATH  failed %s errno=%d pid=%d\n",
               l_path, errno, OS_GETPID(0)));
      return DBSQL_IOERR_LOCK;
    }
    len = strlcat(l_path, "dbsqlplocks", max_len);  
  }
# else
  len = strlcpy(l_path, "/tmp/", max_len);
# endif
#endif

  if( l_path[len-1]!='/' ){
    len = strlcat(l_path, "/", max_len);
  }

  /* transform the db path to a unique cache name */
  db_len = (int)strlen(db_path);
  for( i=0; i<db_len && (i+len+7)<(int)max_len; i++){
    char c = db_path[i];
    l_path[i+len] = (c=='/')?'_':c;
  }
  l_path[i+len]='\0';
  strlcat(l_path, ":auto:", max_len);
  OSTRACE(("GETLOCKPATH  proxy lock path=%s pid=%d\n", l_path, OS_GETPID(0)));
  return DBSQL_OK;
}

/*
 ** Creates the lock file and any missing directories in lock_path
 */
static int __proxy_create_lock_path(const char *lock_path){
  int i, len;
  char buf[MAXPATHLEN];
  int start = 0;

  assert(lock_path!=NULL);
  /* try to create all the intermediate directories */
  len = (int)strlen(lock_path);
  buf[0] = lock_path[0];
  for( i=1; i<len; i++ ){
    if( lock_path[i] == '/' && (i - start > 0) ){
      /* only mkdir if leaf dir != "." or "/" or ".." */
      if( i-start>2 || (i-start==1 && buf[start] != '.' && buf[start] != '/')
         || (i-start==2 && buf[start] != '.' && buf[start+1] != '.') ){
        buf[i]='\0';
        if( OS_MKDIR(buf, DBSQL_DEFAULT_PROXYDIR_PERMISSIONS) ){
          int err=errno;
          if( err!=EEXIST ) {
            OSTRACE(("CREATELOCKPATH  FAILED creating %s, "
                     "'%s' proxy lock path=%s pid=%d\n",
                     buf, strerror(err), lock_path, OS_GETPID(0)));
            return err;
          }
        }
      }
      start=i+1;
    }
    buf[i] = lock_path[i];
  }
  OSTRACE(("CREATELOCKPATH  proxy lock path=%s pid=%d\n",lock_path,OS_GETPID(0)));
  return 0;
}

/*
** Create a new VFS file descriptor (stored in memory obtained from
** dbsql_malloc) and open the file named "path" in the file descriptor.
**
** The caller is responsible not only for closing the file descriptor
** but also for freeing the memory associated with the file descriptor.
*/
static int __proxy_create_unix_file(
    const char *path,        /* path for the new unix_file */
    unix_file **file,       /* unix_file created and returned by ref */
    int islockfile           /* if non zero missing dirs will be created */
) {
  int fd = -1;
  unix_file *p_new;
  int rc = DBSQL_OK;
  int open_flags = O_RDWR | O_CREAT | O_NOFOLLOW;
  dbsql_vfs_t dummy_vfs;
  int terrno = 0;
  unix_unused_fd_t *unused = NULL;

  /* 1. first try to open/create the file
  ** 2. if that fails, and this is a lock file (not-conch), try creating
  ** the parent directories and then try again.
  ** 3. if that fails, try to open the file read-only
  ** otherwise return BUSY (if lock file) or CANTOPEN for the conch file
  */
  unused = __find_reusable_fd(path, open_flags);
  if( unused ){
    fd = unused->fd;
  }else{
    unused = dbsql_malloc64(sizeof(*unused));
    if( !unused ){
      return DBSQL_NOMEM_BKPT;
    }
  }
  if( fd<0 ){
    fd = robust_open(path, open_flags, 0);
    terrno = errno;
    if( fd<0 && errno==ENOENT && islockfile ){
      if( __proxy_create_lock_path(path) == DBSQL_OK ){
        fd = robust_open(path, open_flags, 0);
      }
    }
  }
  if( fd<0 ){
    open_flags = O_RDONLY | O_NOFOLLOW;
    fd = robust_open(path, open_flags, 0);
    terrno = errno;
  }
  if( fd<0 ){
    if( islockfile ){
      return DBSQL_BUSY;
    }
    switch (terrno) {
      case EACCES:
        return DBSQL_PERM;
      case EIO:
        return DBSQL_IOERR_LOCK; /* even though it is the conch */
      default:
        return DBSQL_CANTOPEN_BKPT;
    }
  }

  p_new = (unix_file *)dbsql_malloc64(sizeof(*p_new));
  if( p_new==NULL ){
    rc = DBSQL_NOMEM_BKPT;
    goto end_create_proxy;
  }
  memset(p_new, 0, sizeof(unix_file));
  p_new->open_flags = open_flags;
  memset(&dummy_vfs, 0, sizeof(dummy_vfs));
  dummy_vfs.app_data = (void*)&autolock_io_finder;
  dummy_vfs.name = "dummy";
  unused->fd = fd;
  unused->flags = open_flags;
  p_new->preallocated_unused = unused;

  rc = __fill_in_unix_file(&dummy_vfs, fd, (dbsql_file_t*)p_new, path, 0);
  if( rc==DBSQL_OK ){
    *file = p_new;
    return DBSQL_OK;
  }
end_create_proxy:  
  robust_close(p_new, fd, __LINE__);
  dbsql_free(p_new);
  dbsql_free(unused);
  return rc;
}

#ifdef DBSQL_TEST
/* simulate multiple hosts by creating unique hostid file paths */
int dbsql_hostid_num = 0;
#endif

#define PROXY_HOSTIDLEN    16  /* conch file host id length */

#if HAVE_GETHOSTUUID
/* Not always defined in the headers as it ought to be */
extern int gethostuuid(uuid_t id, const struct timespec *wait);
#endif

/* get the host ID via gethostuuid(), host_id must point to PROXY_HOSTIDLEN
** bytes of writable memory.
*/
static int __proxy_get_host_id(unsigned char *host_id, int *error){
  assert(PROXY_HOSTIDLEN == sizeof(uuid_t));
  memset(host_id, 0, PROXY_HOSTIDLEN);
#if HAVE_GETHOSTUUID
  {
    struct timespec timeout = {1, 0}; /* 1 sec timeout */
    if( gethostuuid(host_id, &timeout) ){
      int err = errno;
      if( error ){
        *error = err;
      }
      return DBSQL_IOERR;
    }
  }
#else
  UNUSED_PARAMETER(error);
#endif
#ifdef DBSQL_TEST
  /* simulate multiple hosts by creating unique hostid file paths */
  if( dbsql_hostid_num != 0){
    host_id[0] = (char)(host_id[0] + (char)(dbsql_hostid_num & 0xFF));
  }
#endif

  return DBSQL_OK;
}

/* The conch file contains the header, host id and lock file path
 */
#define PROXY_CONCHVERSION 2   /* 1-byte header, 16-byte host id, path */
#define PROXY_HEADERLEN    1   /* conch file header length */
#define PROXY_PATHINDEX    (PROXY_HEADERLEN+PROXY_HOSTIDLEN)
#define PROXY_MAXCONCHLEN  (PROXY_HEADERLEN+PROXY_HOSTIDLEN+MAXPATHLEN)

/*
** Takes an open conch file, copies the contents to a new path and then moves
** it back.  The newly created file's file descriptor is assigned to the
** conch file structure and finally the original conch file descriptor is
** closed.  Returns zero if successful.
*/
static int __proxy_break_conch_lock(unix_file *file, uuid_t my_host_id){
  proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
  unix_file *conch_file = ctx->conch_file;
  char t_path[MAXPATHLEN];
  char buf[PROXY_MAXCONCHLEN];
  char *c_path = ctx->conch_file_path;
  size_t read_len = 0;
  size_t path_len = 0;
  char errmsg[64] = "";
  int fd = -1;
  int rc = -1;
  UNUSED_PARAMETER(my_host_id);

  /* create a new path by replace the trailing '-conch' with '-break' */
  path_len = strlcpy(t_path, c_path, MAXPATHLEN);
  if( path_len>MAXPATHLEN || path_len<6 ||
     (strlcpy(&t_path[path_len-5], "break", 6) != 5) ){
    dbsql_snprintf(sizeof(errmsg),errmsg,"path error (len %d)",(int)path_len);
    goto end_breaklock;
  }
  /* read the conch content */
  read_len = OS_PREAD(conch_file->h, buf, PROXY_MAXCONCHLEN, 0);
  if( read_len<PROXY_PATHINDEX ){
    dbsql_snprintf(sizeof(errmsg),errmsg,"read error (len %d)",(int)read_len);
    goto end_breaklock;
  }
  /* write it out to the temporary break file */
  fd = robust_open(t_path, (O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW), 0);
  if( fd<0 ){
    dbsql_snprintf(sizeof(errmsg), errmsg, "create failed (%d)", errno);
    goto end_breaklock;
  }
  if( OS_PWRITE(fd, buf, read_len, 0) != (ssize_t)read_len ){
    dbsql_snprintf(sizeof(errmsg), errmsg, "write failed (%d)", errno);
    goto end_breaklock;
  }
  if( rename(t_path, c_path) ){
    dbsql_snprintf(sizeof(errmsg), errmsg, "rename failed (%d)", errno);
    goto end_breaklock;
  }
  rc = 0;
  fprintf(stderr, "broke stale lock on %s\n", c_path);
  robust_close(file, conch_file->h, __LINE__);
  conch_file->h = fd;
  conch_file->open_flags = O_RDWR | O_CREAT;

end_breaklock:
  if( rc ){
    if( fd>=0 ){
      OS_UNLINK(t_path);
      robust_close(file, fd, __LINE__);
    }
    fprintf(stderr, "failed to break stale lock on %s, %s\n", c_path, errmsg);
  }
  return rc;
}

/* Take the requested lock on the conch file and break a stale lock if the
** host id matches.
*/
static int __proxy_conch_lock(unix_file *file, uuid_t my_host_id, int lock_type){
  proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
  unix_file *conch_file = ctx->conch_file;
  int rc = DBSQL_OK;
  int n_tries = 0;
  struct timespec conch_mod_time;

  memset(&conch_mod_time, 0, sizeof(conch_mod_time));
  do {
    rc = conch_file->method->lock((dbsql_file_t*)conch_file, lock_type);
    n_tries ++;
    if( rc==DBSQL_BUSY ){
      /* If the lock failed (busy):
       * 1st try: get the mod time of the conch, wait 0.5s and try again.
       * 2nd try: fail if the mod time changed or host id is different, wait
       *           10 sec and try again
       * 3rd try: break the lock unless the mod time has changed.
       */
      struct stat buf;
      if( OS_FSTAT(conch_file->h, &buf) ){
        __store_last_errno(file, errno);
        return DBSQL_IOERR_LOCK;
      }
    
      if( n_tries==1 ){
        conch_mod_time = buf.st_mtimespec;
        __unix_sleep(0,500000); /* wait 0.5 sec and try the lock again*/
        continue;
      }

      assert( n_tries>1 );
      if( conch_mod_time.tv_sec != buf.st_mtimespec.tv_sec ||
         conch_mod_time.tv_nsec != buf.st_mtimespec.tv_nsec ){
        return DBSQL_BUSY;
      }
    
      if( n_tries==2 ){
        char t_buf[PROXY_MAXCONCHLEN];
        int len = OS_PREAD(conch_file->h, t_buf, PROXY_MAXCONCHLEN, 0);
        if( len<0 ){
          __store_last_errno(file, errno);
          return DBSQL_IOERR_LOCK;
        }
        if( len>PROXY_PATHINDEX && t_buf[0]==(char)PROXY_CONCHVERSION){
          /* don't break the lock if the host id doesn't match */
          if( 0!=memcmp(&t_buf[PROXY_HEADERLEN], my_host_id, PROXY_HOSTIDLEN) ){
            return DBSQL_BUSY;
          }
        }else{
          /* don't break the lock on short read or a version mismatch */
          return DBSQL_BUSY;
        }
        __unix_sleep(0,10000000); /* wait 10 sec and try the lock again */
        continue;
      }
    
      assert( n_tries==3 );
      if( 0==__proxy_break_conch_lock(file, my_host_id) ){
        rc = DBSQL_OK;
        if( lock_type==EXCLUSIVE_LOCK ){
          rc = conch_file->method->lock((dbsql_file_t*)conch_file, SHARED_LOCK);
        }
        if( !rc ){
          rc = conch_file->method->lock((dbsql_file_t*)conch_file, lock_type);
        }
      }
    }
  } while( rc==DBSQL_BUSY && n_tries<3 );

  return rc;
}

/* Takes the conch by taking a shared lock and read the contents conch, if
** lock_path is non-NULL, the host ID and lock file path must match.  A NULL
** lock_path means that the lock_path in the conch file will be used if the
** host IDs match, or a new lock path will be generated automatically
** and written to the conch file.
*/
static int __proxy_take_conch(unix_file *file){
  proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;

  if( ctx->conch_held!=0 ){
    return DBSQL_OK;
  }else{
    unix_file *conch_file = ctx->conch_file;
    uuid_t my_host_id;
    int error = 0;
    char read_buf[PROXY_MAXCONCHLEN];
    char lock_path[MAXPATHLEN];
    char *temp_lock_path = NULL;
    int rc = DBSQL_OK;
    int create_conch = 0;
    int host_id_match = 0;
    int read_len = 0;
    int try_old_lock_path = 0;
    int force_new_lock_path = 0;
  
    OSTRACE(("TAKECONCH  %d for %s pid=%d\n", conch_file->h,
             (ctx->lock_proxy_path ? ctx->lock_proxy_path : ":auto:"),
             OS_GETPID(0)));

    rc = __proxy_get_host_id(my_host_id, &error);
    if( (rc&0xff)==DBSQL_IOERR ){
      __store_last_errno(file, error);
      goto end_takeconch;
    }
    rc = __proxy_conch_lock(file, my_host_id, SHARED_LOCK);
    if( rc!=DBSQL_OK ){
      goto end_takeconch;
    }
    /* read the existing conch file */
    read_len = __seek_and_read((unix_file*)conch_file, 0, read_buf, PROXY_MAXCONCHLEN);
    if( read_len<0 ){
      /* I/O error: last_errno set by __seek_and_read */
      __store_last_errno(file, conch_file->last_errno);
      rc = DBSQL_IOERR_READ;
      goto end_takeconch;
    }else if( read_len<=(PROXY_HEADERLEN+PROXY_HOSTIDLEN) ||
             read_buf[0]!=(char)PROXY_CONCHVERSION ){
      /* a short read or version format mismatch means we need to create a new
      ** conch file.
      */
      create_conch = 1;
    }
    /* if the host id matches and the lock path already exists in the conch
    ** we'll try to use the path there, if we can't open that path, we'll
    ** retry with a new auto-generated path
    */
    do { /* in case we need to try again for an :auto: named lock file */

      if( !create_conch && !force_new_lock_path ){
        host_id_match = !memcmp(&read_buf[PROXY_HEADERLEN], my_host_id,
                                  PROXY_HOSTIDLEN);
        /* if the conch has data compare the contents */
        if( !ctx->lock_proxy_path ){
          /* for auto-named local lock file, just check the host ID and we'll
           ** use the local lock file path that's already in there
           */
          if( host_id_match ){
            size_t path_len = (read_len - PROXY_PATHINDEX);
          
            if( path_len>=MAXPATHLEN ){
              path_len=MAXPATHLEN-1;
            }
            memcpy(lock_path, &read_buf[PROXY_PATHINDEX], path_len);
            lock_path[path_len] = 0;
            temp_lock_path = lock_path;
            try_old_lock_path = 1;
            /* create a copy of the lock path if the conch is taken */
            goto end_takeconch;
          }
        }else if( host_id_match
               && !strncmp(ctx->lock_proxy_path, &read_buf[PROXY_PATHINDEX],
                           read_len-PROXY_PATHINDEX)
        ){
          /* conch host and lock path match */
          goto end_takeconch;
        }
      }
    
      /* if the conch isn't writable and doesn't match, we can't take it */
      if( (conch_file->open_flags&O_RDWR) == 0 ){
        rc = DBSQL_BUSY;
        goto end_takeconch;
      }
    
      /* either the conch didn't match or we need to create a new one */
      if( !ctx->lock_proxy_path ){
        __proxy_get_lock_path(ctx->db_path, lock_path, MAXPATHLEN);
        temp_lock_path = lock_path;
        /* create a copy of the lock path _only_ if the conch is taken */
      }
    
      /* update conch with host and path (this will fail if other process
      ** has a shared lock already), if the host id matches, use the big
      ** stick.
      */
      futimes(conch_file->h, NULL);
      if( host_id_match && !create_conch ){
        if( conch_file->inode && conch_file->inode->n_shared>1 ){
          /* We are trying for an exclusive lock but another thread in this
           ** same process is still holding a shared lock. */
          rc = DBSQL_BUSY;
        } else {        
          rc = __proxy_conch_lock(file, my_host_id, EXCLUSIVE_LOCK);
        }
      }else{
        rc = __proxy_conch_lock(file, my_host_id, EXCLUSIVE_LOCK);
      }
      if( rc==DBSQL_OK ){
        char write_buffer[PROXY_MAXCONCHLEN];
        int write_size = 0;
      
        write_buffer[0] = (char)PROXY_CONCHVERSION;
        memcpy(&write_buffer[PROXY_HEADERLEN], my_host_id, PROXY_HOSTIDLEN);
        if( ctx->lock_proxy_path!=NULL ){
          strlcpy(&write_buffer[PROXY_PATHINDEX], ctx->lock_proxy_path,
                  MAXPATHLEN);
        }else{
          strlcpy(&write_buffer[PROXY_PATHINDEX], temp_lock_path, MAXPATHLEN);
        }
        write_size = PROXY_PATHINDEX + strlen(&write_buffer[PROXY_PATHINDEX]);
        robust_ftruncate(conch_file->h, write_size);
        rc = __unix_write((dbsql_file_t *)conch_file, write_buffer, write_size, 0);
        full_fsync(conch_file->h,0,0);
        /* If we created a new conch file (not just updated the contents of a
         ** valid conch file), try to match the permissions of the database
         */
        if( rc==DBSQL_OK && create_conch ){
          struct stat buf;
          int err = OS_FSTAT(file->h, &buf);
          if( err==0 ){
            mode_t cmode = buf.st_mode&(S_IRUSR|S_IWUSR | S_IRGRP|S_IWGRP |
                                        S_IROTH|S_IWOTH);
            /* try to match the database file R/W permissions, ignore failure */
#ifndef DBSQL_PROXY_DEBUG
            OS_FCHMOD(conch_file->h, cmode);
#else
            do{
              rc = OS_FCHMOD(conch_file->h, cmode);
            }while( rc==(-1) && errno==EINTR );
            if( rc!=0 ){
              int code = errno;
              fprintf(stderr, "fchmod %o FAILED with %d %s\n",
                      cmode, code, strerror(code));
            } else {
              fprintf(stderr, "fchmod %o SUCCEDED\n",cmode);
            }
          }else{
            int code = errno;
            fprintf(stderr, "STAT FAILED[%d] with %d %s\n",
                    err, code, strerror(code));
#endif
          }
        }
      }
      conch_file->method->unlock((dbsql_file_t*)conch_file, SHARED_LOCK);
    
    end_takeconch:
      OSTRACE(("TRANSPROXY: CLOSE  %d\n", file->h));
      if( rc==DBSQL_OK && file->open_flags ){
        int fd;
        if( file->h>=0 ){
          robust_close(file, file->h, __LINE__);
        }
        file->h = -1;
        fd = robust_open(ctx->db_path, file->open_flags, 0);
        OSTRACE(("TRANSPROXY: OPEN  %d\n", fd));
        if( fd>=0 ){
          file->h = fd;
        }else{
          rc=DBSQL_CANTOPEN_BKPT; /* DBSQL_BUSY? __proxy_take_conch called
           during locking */
        }
      }
      if( rc==DBSQL_OK && !ctx->lock_proxy ){
        char *path = temp_lock_path ? temp_lock_path : ctx->lock_proxy_path;
        rc = __proxy_create_unix_file(path, &ctx->lock_proxy, 1);
        if( rc!=DBSQL_OK && rc!=DBSQL_NOMEM && try_old_lock_path ){
          /* we couldn't create the proxy lock file with the old lock file path
           ** so try again via auto-naming
           */
          force_new_lock_path = 1;
          try_old_lock_path = 0;
          continue; /* go back to the do {} while start point, try again */
        }
      }
      if( rc==DBSQL_OK ){
        /* Need to make a copy of path if we extracted the value
         ** from the conch file or the path was allocated on the stack
         */
        if( temp_lock_path ){
          ctx->lock_proxy_path = __db_str_dup(0, temp_lock_path);
          if( !ctx->lock_proxy_path ){
            rc = DBSQL_NOMEM_BKPT;
          }
        }
      }
      if( rc==DBSQL_OK ){
        ctx->conch_held = 1;
      
        if( ctx->lock_proxy->method == &afp_io_methods ){
          afp_locking_context *afp_ctx;
          afp_ctx = (afp_locking_context *)ctx->lock_proxy->locking_context;
          afp_ctx->db_path = ctx->lock_proxy_path;
        }
      } else {
        conch_file->method->unlock((dbsql_file_t*)conch_file, NO_LOCK);
      }
      OSTRACE(("TAKECONCH  %d %s\n", conch_file->h,
               rc==DBSQL_OK?"ok":"failed"));
      return rc;
    } while (1); /* in case we need to retry the :auto: lock file -
                 ** we should never get here except via the 'continue' call. */
  }
}

/*
** If file holds a lock on a conch file, then release that lock.
*/
static int __proxy_release_conch(unix_file *file){
  int rc = DBSQL_OK;         /* Subroutine return code */
  proxy_locking_context *ctx;  /* The locking context for the proxy lock */
  unix_file *conch_file;        /* Name of the conch file */

  ctx = (proxy_locking_context *)file->locking_context;
  conch_file = ctx->conch_file;
  OSTRACE(("RELEASECONCH  %d for %s pid=%d\n", conch_file->h,
           (ctx->lock_proxy_path ? ctx->lock_proxy_path : ":auto:"),
           OS_GETPID(0)));
  if( ctx->conch_held>0 ){
    rc = conch_file->method->unlock((dbsql_file_t*)conch_file, NO_LOCK);
  }
  ctx->conch_held = 0;
  OSTRACE(("RELEASECONCH  %d %s\n", conch_file->h,
           (rc==DBSQL_OK ? "ok" : "failed")));
  return rc;
}

/*
** Given the name of a database file, compute the name of its conch file.
** Store the conch filename in memory obtained from dbsql_malloc64().
** Make *conch_path point to the new name.  Return DBSQL_OK on success
** or DBSQL_NOMEM if unable to obtain memory.
**
** The caller is responsible for ensuring that the allocated memory
** space is eventually freed.
**
** *conch_path is set to NULL if a memory allocation error occurs.
*/
static int __proxy_create_conch_pathname(char *db_path, char **p_conch_path){
  int i;                        /* Loop counter */
  int len = (int)strlen(db_path); /* Length of database filename - db_path */
  char *conch_path;              /* buffer in which to construct conch name */

  /* Allocate space for the conch filename and initialize the name to
  ** the name of the original database file. */
  *p_conch_path = conch_path = (char *)dbsql_malloc64(len + 8);
  if( conch_path==0 ){
    return DBSQL_NOMEM_BKPT;
  }
  memcpy(conch_path, db_path, len+1);

  /* now insert a "." before the last / character */
  for( i=(len-1); i>=0; i-- ){
    if( conch_path[i]=='/' ){
      i++;
      break;
    }
  }
  conch_path[i]='.';
  while ( i<len ){
    conch_path[i+1]=db_path[i];
    i++;
  }

  /* append the "-conch" suffix to the file */
  memcpy(&conch_path[i+1], "-conch", 7);
  assert( (int)strlen(conch_path) == len+7 );

  return DBSQL_OK;
}


/* Takes a fully configured proxy locking-style unix file and switches
** the local lock file path
*/
static int __switch_lock_proxy_path(unix_file *file, const char *path) {
  proxy_locking_context *ctx = (proxy_locking_context*)file->locking_context;
  char *old_path = ctx->lock_proxy_path;
  int rc = DBSQL_OK;

  if( file->file_lock!=NO_LOCK ){
    return DBSQL_BUSY;
  }

  /* nothing to do if the path is NULL, :auto: or matches the existing path */
  if( !path || path[0]=='\0' || !strcmp(path, ":auto:") ||
    (old_path && !strncmp(old_path, path, MAXPATHLEN)) ){
    return DBSQL_OK;
  }else{
    unix_file *lock_proxy = ctx->lock_proxy;
    ctx->lock_proxy=NULL;
    ctx->conch_held = 0;
    if( lock_proxy!=NULL ){
      rc=lock_proxy->method->x_close((dbsql_file_t *)lock_proxy);
      if( rc ) return rc;
      dbsql_free(lock_proxy);
    }
    dbsql_free(old_path);
    ctx->lock_proxy_path = __db_str_dup(0, path);
  }

  return rc;
}

/*
** file is a file that has been opened by a prior x_open call.  db_path
** is a string buffer at least MAXPATHLEN+1 characters in size.
**
** This routine find the filename associated with file and writes it
** int db_path.
*/
static int __proxy_get_db_path_for_unix_file(unix_file *file, char *db_path){
#if defined(__APPLE__)
  if( file->method == &afp_io_methods ){
    /* afp style keeps a reference to the db path in the file_path field
    ** of the struct */
    assert( (int)strlen((char*)file->locking_context)<=MAXPATHLEN );
    strlcpy(db_path, ((afp_locking_context *)file->locking_context)->db_path,
            MAXPATHLEN);
  } else
#endif
  if( file->method == &dotlock_io_methods ){
    /* dot lock style uses the locking context to store the dot lock
    ** file path */
    int len = strlen((char *)file->locking_context) - strlen(DOTLOCK_SUFFIX);
    memcpy(db_path, (char *)file->locking_context, len + 1);
  }else{
    /* all other styles use the locking context to store the db file path */
    assert( strlen((char*)file->locking_context)<=MAXPATHLEN );
    strlcpy(db_path, (char *)file->locking_context, MAXPATHLEN);
  }
  return DBSQL_OK;
}

/*
** Takes an already filled in unix file and alters it so all file locking
** will be performed on the local proxy lock file.  The following fields
** are preserved in the locking context so that they can be restored and
** the unix structure properly cleaned up at close time:
**  ->locking_context
**  ->method
*/
static int __proxy_transform_unix_file(unix_file *file, const char *path) {
  proxy_locking_context *ctx;
  char db_path[MAXPATHLEN+1];       /* Name of the database file */
  char *lock_path=NULL;
  int rc = DBSQL_OK;

  if( file->file_lock!=NO_LOCK ){
    return DBSQL_BUSY;
  }
  __proxy_get_db_path_for_unix_file(file, db_path);
  if( !path || path[0]=='\0' || !strcmp(path, ":auto:") ){
    lock_path=NULL;
  }else{
    lock_path=(char *)path;
  }

  OSTRACE(("TRANSPROXY  %d for %s pid=%d\n", file->h,
           (lock_path ? lock_path : ":auto:"), OS_GETPID(0)));

  ctx = dbsql_malloc64( sizeof(*ctx) );
  if( ctx==0 ){
    return DBSQL_NOMEM_BKPT;
  }
  memset(ctx, 0, sizeof(*ctx));

  rc = __proxy_create_conch_pathname(db_path, &ctx->conch_file_path);
  if( rc==DBSQL_OK ){
    rc = __proxy_create_unix_file(ctx->conch_file_path, &ctx->conch_file, 0);
    if( rc==DBSQL_CANTOPEN && ((file->open_flags&O_RDWR) == 0) ){
      /* if (a) the open flags are not O_RDWR, (b) the conch isn't there, and
      ** (c) the file system is read-only, then enable no-locking access.
      ** Ugh, since O_RDONLY==0x0000 we test for !O_RDWR since __unix_open asserts
      ** that open_flags will have only one of O_RDONLY or O_RDWR.
      */
      struct statfs fs_info;
      struct stat conch_info;
      int go_lockless = 0;

      if( OS_STAT(ctx->conch_file_path, &conch_info) == -1 ) {
        int err = errno;
        if( (err==ENOENT) && (statfs(db_path, &fs_info) != -1) ){
          go_lockless = (fs_info.f_flags&MNT_RDONLY) == MNT_RDONLY;
        }
      }
      if( go_lockless ){
        ctx->conch_held = -1; /* read only FS/ lockless */
        rc = DBSQL_OK;
      }
    }
  }
  if( rc==DBSQL_OK && lock_path ){
    ctx->lock_proxy_path = __db_str_dup(0, lock_path);
  }

  if( rc==DBSQL_OK ){
    ctx->db_path = __db_str_dup(0, db_path);
    if( ctx->db_path==NULL ){
      rc = DBSQL_NOMEM_BKPT;
    }
  }
  if( rc==DBSQL_OK ){
    /* all memory is allocated, proxys are created and assigned,
    ** switch the locking context and method then return.
    */
    ctx->old_locking_context = file->locking_context;
    file->locking_context = ctx;
    ctx->old_method = file->method;
    file->method = &proxy_io_methods;
  }else{
    if( ctx->conch_file ){
      ctx->conch_file->method->x_close((dbsql_file_t *)ctx->conch_file);
      dbsql_free(ctx->conch_file);
    }
    __dbsql_db_free(0, ctx->lock_proxy_path);
    dbsql_free(ctx->conch_file_path);
    dbsql_free(ctx);
  }
  OSTRACE(("TRANSPROXY  %d %s\n", file->h,
           (rc==DBSQL_OK ? "ok" : "failed")));
  return rc;
}


/*
** This routine handles dbsql_file_control() calls that are specific
** to proxy locking.
*/
static int __proxy_file_control(dbsql_file_t *id, int op, void *arg){
  switch( op ){
    case DBSQL_FCNTL_GET_LOCKPROXYFILE: {
      unix_file *file = (unix_file*)id;
      if( file->method == &proxy_io_methods ){
        proxy_locking_context *ctx = (proxy_locking_context*)file->locking_context;
        __proxy_take_conch(file);
        if( ctx->lock_proxy_path ){
          *(const char **)arg = ctx->lock_proxy_path;
        }else{
          *(const char **)arg = ":auto: (not held)";
        }
      } else {
        *(const char **)arg = NULL;
      }
      return DBSQL_OK;
    }
    case DBSQL_FCNTL_SET_LOCKPROXYFILE: {
      unix_file *file = (unix_file*)id;
      int rc = DBSQL_OK;
      int is_proxy_style = (file->method == &proxy_io_methods);
      if( arg==NULL || (const char *)arg==0 ){
        if( is_proxy_style ){
          /* turn off proxy locking - not supported.  If support is added for
          ** switching proxy locking mode off then it will need to fail if
          ** the journal mode is WAL mode.
          */
          rc = DBSQL_ERROR /*DBSQL_PROTOCOL? DBSQL_MISUSE?*/;
        }else{
          /* turn off proxy locking - already off - NOOP */
          rc = DBSQL_OK;
        }
      }else{
        const char *proxy_path = (const char *)arg;
        if( is_proxy_style ){
          proxy_locking_context *ctx =
            (proxy_locking_context*)file->locking_context;
          if( !strcmp(arg, ":auto:")
           || (ctx->lock_proxy_path &&
               !strncmp(ctx->lock_proxy_path, proxy_path, MAXPATHLEN))
          ){
            rc = DBSQL_OK;
          }else{
            rc = __switch_lock_proxy_path(file, proxy_path);
          }
        }else{
          /* turn on proxy file locking */
          rc = __proxy_transform_unix_file(file, proxy_path);
        }
      }
      return rc;
    }
    default: {
      assert( 0 );  /* The call assures that only valid opcodes are sent */
    }
  }
  /*NOTREACHED*/ assert(0);
  return DBSQL_ERROR;
}

/*
** Within this division (the proxying locking implementation) the procedures
** above this point are all utilities.  The lock-related methods of the
** proxy-locking dbsql_io_method object follow.
*/


/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *res_out
** to a non-zero value otherwise *res_out is set to zero.  The return value
** is set to DBSQL_OK unless an I/O error occurs during lock checking.
*/
static int __proxy_check_reserved_lock(dbsql_file_t *id, int *res_out) {
  unix_file *file = (unix_file*)id;
  int rc = __proxy_take_conch(file);
  if( rc==DBSQL_OK ){
    proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
    if( ctx->conch_held>0 ){
      unix_file *proxy = ctx->lock_proxy;
      return proxy->method->check_reserved_lock((dbsql_file_t*)proxy, res_out);
    }else{ /* conch_held < 0 is lockless */
      res_out=0;
    }
  }
  return rc;
}

/*
** Lock the file with the lock specified by parameter file_lock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the __dbsql_os_unlock()
** routine to lower a locking level.
*/
static int __proxy_lock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  int rc = __proxy_take_conch(file);
  if( rc==DBSQL_OK ){
    proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
    if( ctx->conch_held>0 ){
      unix_file *proxy = ctx->lock_proxy;
      rc = proxy->method->lock((dbsql_file_t*)proxy, file_lock);
      file->file_lock = proxy->file_lock;
    }else{
      /* conch_held < 0 is lockless */
    }
  }
  return rc;
}


/*
** Lower the locking level on file descriptor file to file_lock.  file_lock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int __proxy_unlock(dbsql_file_t *id, int file_lock) {
  unix_file *file = (unix_file*)id;
  int rc = __proxy_take_conch(file);
  if( rc==DBSQL_OK ){
    proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
    if( ctx->conch_held>0 ){
      unix_file *proxy = ctx->lock_proxy;
      rc = proxy->method->unlock((dbsql_file_t*)proxy, file_lock);
      file->file_lock = proxy->file_lock;
    }else{
      /* conch_held < 0 is lockless */
    }
  }
  return rc;
}

/*
** Close a file that uses proxy locks.
*/
static int __proxy_close(dbsql_file_t *id) {
  if( ALWAYS(id) ){
    unix_file *file = (unix_file*)id;
    proxy_locking_context *ctx = (proxy_locking_context *)file->locking_context;
    unix_file *lock_proxy = ctx->lock_proxy;
    unix_file *conch_file = ctx->conch_file;
    int rc = DBSQL_OK;

    if( lock_proxy ){
      rc = lock_proxy->method->unlock((dbsql_file_t*)lock_proxy, NO_LOCK);
      if( rc ) return rc;
      rc = lock_proxy->method->x_close((dbsql_file_t*)lock_proxy);
      if( rc ) return rc;
      dbsql_free(lock_proxy);
      ctx->lock_proxy = 0;
    }
    if( conch_file ){
      if( ctx->conch_held ){
        rc = __proxy_release_conch(file);
        if( rc ) return rc;
      }
      rc = conch_file->method->x_close((dbsql_file_t*)conch_file);
      if( rc ) return rc;
      dbsql_free(conch_file);
    }
    __dbsql_db_free(0, ctx->lock_proxy_path);
    dbsql_free(ctx->conch_file_path);
    __dbsql_db_free(0, ctx->db_path);
    /* restore the original locking context and method then close it */
    file->locking_context = ctx->old_locking_context;
    file->method = ctx->old_method;
    dbsql_free(ctx);
    return file->method->x_close(id);
  }
  return DBSQL_OK;
}



#endif /* defined(__APPLE__) && DBSQL_ENABLE_LOCKING_STYLE */
/*
** The proxy locking style is intended for use with AFP filesystems.
** And since AFP is only supported on __mac_osx, the proxy locking is also
** restricted to __mac_osx.
**
**
******************* End of the proxy lock implementation **********************
******************************************************************************/

/*
** Initialize the operating system interface.
**
** This routine registers all VFS implementations for unix-like operating
** systems.  This routine, and the dbsql_os_end() routine that follows,
** should be the only routines in this file that are visible from other
** files.
**
** This routine is called once during dbsql initialization and by a
** single thread.  The memory allocation and mutex subsystems have not
** necessarily been initialized when this routine is called, and so they
** should not be used.
*/
int dbsql_os_init(void){
  /*
  ** The following macro defines an initializer for an dbsql_vfs_t object.
  ** The name of the VFS is NAME.  The app_data is a pointer to a pointer
  ** to the "finder" function.  (app_data is a pointer to a pointer because
  ** silly C90 rules prohibit a void* from being cast to a function pointer
  ** and so we have to go through the intermediate pointer to avoid problems
  ** when compiling with -pedantic-errors on GCC.)
  **
  ** The FINDER parameter to this macro is the name of the pointer to the
  ** finder-function.  The finder-function returns a pointer to the
  ** dbsql_io_methods object that implements the desired locking
  ** behaviors.  See the division above that contains the IOMETHODS
  ** macro for addition information on finder-functions.
  **
  ** Most finders simply return a pointer to a fixed dbsql_io_methods_t
  ** object.  But the "autolock_io_finder" available on __mac_osx does a little
  ** more than that; it looks at the filesystem type that hosts the
  ** database file and tries to choose an locking method appropriate for
  ** that filesystem time.
  */
  #define UNIXVFS(VFSNAME, FINDER) {                        \
    3,                    /* i_version */                    \
    sizeof(unix_file),     /* sz_os_file */                    \
    MAX_PATHNAME,         /* mx_pathname */                  \
    0,                    /* next */                       \
    VFSNAME,              /* name */                       \
    (void*)&FINDER,       /* app_data */                    \
    __unix_open,             /* x_open */                       \
    __unix_delete,           /* x_delete */                     \
    __unix_access,           /* x_access */                     \
    __unix_full_pathname,     /* full_pathname */               \
    UNIX_DL_OPEN,           /* dl_open */                     \
    UNIX_DL_ERROR,          /* dl_error */                    \
    UNIX_DL_SYM,            /* dl_sym */                      \
    UNIX_DL_CLOSE,          /* dl_close */                    \
    __unix_randomness,       /* randomness */                 \
    __unix_sleep,            /* x_sleep */                      \
    UNIX_CURRENT_TIME,      /* current_time */                \
    __unix_get_last_error,     /* get_last_error */               \
    __unix_current_time_int64, /* current_time_int64 */           \
    __unix_set_system_call,    /* set_system_call */              \
    __unix_get_system_call,    /* get_system_call */              \
    __unix_next_system_call,   /* next_system_call */             \
  }

  /*
  ** All default VFSes for unix are contained in the following array.
  **
  ** Note that the dbsql_vfs_t.next field of the VFS object is modified
  ** by the dbsql core when the VFS is registered.  So the following
  ** array cannot be const.
  */
  static dbsql_vfs_t vfses[] = {
#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
    UNIXVFS("unix",          autolock_io_finder ),
#elif OS_VXWORKS
    UNIXVFS("unix",          vxworks_io_finder ),
#else
    UNIXVFS("unix",          posix_io_finder ),
#endif
    UNIXVFS("unix-none",     nolock_io_finder ),
    UNIXVFS("unix-dotfile",  dotlock_io_finder ),
    UNIXVFS("unix-excl",     posix_io_finder ),
#if OS_VXWORKS
    UNIXVFS("unix-namedsem", sem_io_finder ),
#endif
#if DBSQL_ENABLE_LOCKING_STYLE || OS_VXWORKS
    UNIXVFS("unix-posix",    posix_io_finder ),
#endif
#if DBSQL_ENABLE_LOCKING_STYLE
    UNIXVFS("unix-flock",    flock_io_finder ),
#endif
#if DBSQL_ENABLE_LOCKING_STYLE && defined(__APPLE__)
    UNIXVFS("unix-afp",      afp_io_finder ),
    UNIXVFS("unix-nfs",      nfs_io_finder ),
    UNIXVFS("unix-proxy",    proxy_io_finder ),
#endif
  };
  unsigned int i;          /* Loop counter */

  /* Double-check that the __a_syscall[] array has been constructed
  ** correctly.  See ticket [bb3a86e890c8e96ab] */
  assert( ARRAY_SIZE(__a_syscall)==29 );

  /* Register all VFSes defined in the vfses[] array */
  for(i=0; i<(sizeof(vfses)/sizeof(dbsql_vfs_t)); i++){
#ifdef DBSQL_DEFAULT_UNIX_VFS
    dbsql_vfs_register(&vfses[i],
           0==strcmp(vfses[i].name,DBSQL_DEFAULT_UNIX_VFS));
#else
    dbsql_vfs_register(&vfses[i], i==0);
#endif
  }
#ifdef DBSQL_OS_KV_OPTIONAL
  __kvvfs_init();
#endif
  __unix_big_lock = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_VFS1);
#if OS_VXWORKS
  __vxworks_mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_VFS2);
#endif

#ifndef DBSQL_OMIT_WAL
  /* Validate lock assumptions */
  assert( DBSQL_SHM_NLOCK==8 );  /* Number of available locks */
  assert( UNIX_SHM_BASE==120  );  /* Start of locking area */
  /* Locks:
  **    WRITE       UNIX_SHM_BASE      120
  **    CKPT        UNIX_SHM_BASE+1    121
  **    RECOVER     UNIX_SHM_BASE+2    122
  **    READ-0      UNIX_SHM_BASE+3    123
  **    READ-1      UNIX_SHM_BASE+4    124
  **    READ-2      UNIX_SHM_BASE+5    125
  **    READ-3      UNIX_SHM_BASE+6    126
  **    READ-4      UNIX_SHM_BASE+7    127
  **    DMS         UNIX_SHM_BASE+8    128
  */
  assert( UNIX_SHM_DMS==128   );  /* Byte offset of the deadman-switch */
#endif

  /* Initialize temp file dir array. */
  __unix_temp_file_init();

  return DBSQL_OK;
}

/*
** Shutdown the operating system interface.
**
** Some operating systems might need to do some cleanup in this routine,
** to release dynamically allocated objects.  But not on unix.
** This routine is a no-op for unix.
*/
int dbsql_os_end(void){
  __unix_big_lock = 0;
#if OS_VXWORKS
  __vxworks_mutex = 0;
#endif
  return DBSQL_OK;
}

#endif /* DBSQL_OS_UNIX */
