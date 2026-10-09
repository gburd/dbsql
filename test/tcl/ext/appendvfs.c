/*
** 2017-10-20
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
** This file implements a VFS shim that allows an dbsql database to be
** appended onto the end of some other file, such as an executable.
**
** A special record must appear at the end of the file that identifies the
** file as an appended database and provides the offset to the first page
** of the exposed content. (Or, it is the length of the content prefix.)
** For best performance page 1 should be located at a disk page boundary,
** though that is not required.
**
** When opening a database using this VFS, the connection might treat
** the file as an ordinary dbsql database, or it might treat it as a
** database appended onto some other file.  The decision is made by
** applying the following rules in order:
**
**  (1)  An empty file is an ordinary database.
**
**  (2)  If the file ends with the appendvfs trailer string
**       "Start-Of-SQLite3-NNNNNNNN" that file is an appended database.
**
**  (3)  If the file begins with the standard dbsql prefix string
**       "dbsql format 3", that file is an ordinary database.
**
**  (4)  If none of the above apply and the DBSQL_OPEN_CREATE flag is
**       set, then a new database is appended to the already existing file.
**
**  (5)  Otherwise, DBSQL_CANTOPEN is returned.
**
** To avoid unnecessary complications with the PENDING_BYTE, the size of
** the file containing the database is limited to 1GiB. (1073741824 bytes)
** This VFS will not read or write past the 1GiB mark.  This restriction
** might be lifted in future versions.  For now, if you need a larger
** database, then keep it in a separate file.
**
** If the file being opened is a plain database (not an appended one), then
** this shim is a pass-through into the default underlying VFS. (rule 3)
**/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* The append mark at the end of the database is:
**
**     Start-Of-SQLite3-NNNNNNNN
**     123456789 123456789 12345
**
** The NNNNNNNN represents a 64-bit big-endian unsigned integer which is
** the offset to page 1, and also the length of the prefix content.
*/
#define APND_MARK_PREFIX     "Start-Of-dbsql3-"
#define APND_MARK_PREFIX_SZ  17
#define APND_MARK_FOS_SZ      8
#define APND_MARK_SIZE       (APND_MARK_PREFIX_SZ+APND_MARK_FOS_SZ)

/*
** Maximum size of the combined prefix + database + append-mark.  This
** must be less than 0x40000000 to avoid locking issues on Windows.
*/
#define APND_MAX_SIZE  (0x40000000)

/*
** Try to align the database to an even multiple of APND_ROUNDUP bytes.
*/
#ifndef APND_ROUNDUP
#define APND_ROUNDUP 4096
#endif
#define APND_ALIGN_MASK         ((int64_t)(APND_ROUNDUP-1))
#define APND_START_ROUNDUP(fsz) (((fsz)+APND_ALIGN_MASK) & ~APND_ALIGN_MASK)

/*
** Forward declaration of objects used by this utility
*/
typedef struct dbsql_vfs_t apnd_vfs_t;
typedef struct apnd_file_t apnd_file_t;

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p)  ((dbsql_vfs_t*)((p)->app_data))
#define ORIGFILE(p) ((dbsql_file_t*)(((apnd_file_t*)(p))+1))

/* An open appendvfs file
**
** An instance of this structure describes the appended database file.
** A separate dbsql_file_t object is always appended. The appended
** dbsql_file_t object (which can be accessed using ORIGFILE()) describes
** the entire file, including the prefix, the database, and the
** append-mark.
**
** The structure of an __append_vfs database is like this:
**
**   +-------------+---------+----------+-------------+
**   | prefix-file | padding | database | append-mark |
**   +-------------+---------+----------+-------------+
**                           ^          ^
**                           |          |
**                         i_pg_one      i_mark
**
**
** "prefix file" -  file onto which the database has been appended.
** "padding"     -  zero or more bytes inserted so that "database"
**                  starts on an APND_ROUNDUP boundary
** "database"    -  The dbsql database file
** "append-mark" -  The 25-byte "Start-Of-SQLite3-NNNNNNNN" that indicates
**                  the offset from the start of prefix-file to the start
**                  of "database".
**
** The size of the database is i_mark - i_pg_one.
**
** The NNNNNNNN in the "Start-Of-SQLite3-NNNNNNNN" suffix is the value
** of i_pg_one stored as a big-ending 64-bit integer.
**
** i_mark will be the size of the underlying file minus 25 (APND_MARKSIZE).
** Or, i_mark is -1 to indicate that it has not yet been written.
*/
struct apnd_file_t {
  dbsql_file_t base;        /* Subclass.  MUST BE FIRST! */
  int64_t i_pg_one;     /* Offset to the start of the database */
  int64_t i_mark;      /* Offset of the append mark.  -1 if unwritten */
  /* Always followed by another dbsql_file_t that describes the whole file */
};

/*
** Methods for apnd_file_t
*/
static int __apnd_close(dbsql_file_t*);
static int __apnd_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __apnd_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __apnd_truncate(dbsql_file_t*, int64_t size);
static int __apnd_sync(dbsql_file_t*, int flags);
static int __apnd_file_size(dbsql_file_t*, int64_t *size);
static int __apnd_lock(dbsql_file_t*, int);
static int __apnd_unlock(dbsql_file_t*, int);
static int __apnd_check_reserved_lock(dbsql_file_t*, int *res_out);
static int __apnd_file_control(dbsql_file_t*, int op, void *arg);
static int __apnd_sector_size(dbsql_file_t*);
static int __apnd_device_characteristics(dbsql_file_t*);
static int __apnd_shm_map(dbsql_file_t*, int i_pg, int pgsz, int, void volatile**);
static int __apnd_shm_lock(dbsql_file_t*, int offset, int n, int flags);
static void __apnd_shm_barrier(dbsql_file_t*);
static int __apnd_shm_unmap(dbsql_file_t*, int delete_flag);
static int __apnd_fetch(dbsql_file_t*, int64_t i_ofst, int i_amt, void **pp);
static int __apnd_unfetch(dbsql_file_t*, int64_t i_ofst, void *p);

/*
** Methods for apnd_vfs_t
*/
static int __apnd_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __apnd_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __apnd_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __apnd_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
static void *__apnd_dl_open(dbsql_vfs_t*, const char *filename);
static void __apnd_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*apnd_dl_sym(dbsql_vfs_t *vfs, void *p, const char*sym))(void);
static void __apnd_dl_close(dbsql_vfs_t*, void*);
static int __apnd_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __apnd_sleep(dbsql_vfs_t*, int microseconds);
static int __apnd_current_time(dbsql_vfs_t*, double*);
static int __apnd_get_last_error(dbsql_vfs_t*, int, char *);
static int __apnd_current_time_int64(dbsql_vfs_t*, int64_t*);
static int __apnd_set_system_call(dbsql_vfs_t*, const char*,dbsql_syscall_ptr);
static dbsql_syscall_ptr __apnd_get_system_call(dbsql_vfs_t*, const char *z);
static const char *__apnd_next_system_call(dbsql_vfs_t*, const char *name);

static dbsql_vfs_t apnd_vfs = {
  3,                            /* i_version (set when registered) */
  0,                            /* sz_os_file (set when registered) */
  1024,                         /* mx_pathname */
  0,                            /* next */
  "apndvfs",                    /* name */
  0,                            /* app_data (set when registered) */ 
  __apnd_open,                     /* x_open */
  __apnd_delete,                   /* x_delete */
  __apnd_access,                   /* x_access */
  __apnd_full_pathname,             /* full_pathname */
  __apnd_dl_open,                   /* dl_open */
  __apnd_dl_error,                  /* dl_error */
  apnd_dl_sym,                    /* dl_sym */
  __apnd_dl_close,                  /* dl_close */
  __apnd_randomness,               /* randomness */
  __apnd_sleep,                    /* x_sleep */
  __apnd_current_time,              /* current_time */
  __apnd_get_last_error,             /* get_last_error */
  __apnd_current_time_int64,         /* current_time_int64 */
  __apnd_set_system_call,            /* set_system_call */
  __apnd_get_system_call,            /* get_system_call */
  __apnd_next_system_call            /* next_system_call */
};

static const dbsql_io_methods_t apnd_io_methods = {
  3,                              /* i_version */
  __apnd_close,                      /* x_close */
  __apnd_read,                       /* x_read */
  __apnd_write,                      /* x_write */
  __apnd_truncate,                   /* truncate */
  __apnd_sync,                       /* x_sync */
  __apnd_file_size,                   /* file_size */
  __apnd_lock,                       /* lock */
  __apnd_unlock,                     /* unlock */
  __apnd_check_reserved_lock,          /* check_reserved_lock */
  __apnd_file_control,                /* file_control */
  __apnd_sector_size,                 /* sector_size */
  __apnd_device_characteristics,      /* device_characteristics */
  __apnd_shm_map,                     /* shm_map */
  __apnd_shm_lock,                    /* shm_lock */
  __apnd_shm_barrier,                 /* shm_barrier */
  __apnd_shm_unmap,                   /* shm_unmap */
  __apnd_fetch,                      /* fetch */
  __apnd_unfetch                     /* unfetch */
};

/*
** Close an apnd-file.
*/
static int __apnd_close(dbsql_file_t *file){
  file = ORIGFILE(file);
  return file->methods->x_close(file);
}

/*
** Read data from an apnd-file.
*/
static int __apnd_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  apnd_file_t *paf = (apnd_file_t *)file;
  file = ORIGFILE(file);
  return file->methods->x_read(file, buf, i_amt, paf->i_pg_one+i_ofst);
}

/*
** Add the append-mark onto what should become the end of the file.
*  If and only if this succeeds, internal apnd_file_t.i_mark is updated.
*  Parameter i_write_end is the appendvfs-relative offset of the new mark.
*/
static int __apnd_write_mark(
  apnd_file_t *paf,
  dbsql_file_t *file,
  int64_t i_write_end
){
  int64_t i_pg_one = paf->i_pg_one;
  unsigned char a[APND_MARK_SIZE];
  int i = APND_MARK_FOS_SZ;
  int rc;
  assert(file == ORIGFILE(paf));
  memcpy(a, APND_MARK_PREFIX, APND_MARK_PREFIX_SZ);
  while( --i >= 0 ){
    a[APND_MARK_PREFIX_SZ+i] = (unsigned char)(i_pg_one & 0xff);
    i_pg_one >>= 8;
  }
  i_write_end += paf->i_pg_one;
  if( DBSQL_OK==(rc = file->methods->x_write
                  (file, a, APND_MARK_SIZE, i_write_end)) ){
    paf->i_mark = i_write_end;
  }
  return rc;
}

/*
** Write data to an apnd-file.
*/
static int __apnd_write(
  dbsql_file_t *file,
  const void *buf,
  int i_amt,
  int64_t i_ofst
){
  apnd_file_t *paf = (apnd_file_t *)file;
  int64_t i_write_end = i_ofst + i_amt;
  if( i_write_end>=APND_MAX_SIZE ) return DBSQL_FULL;
  file = ORIGFILE(file);
  /* If append-mark is absent or will be overwritten, write it. */
  if( paf->i_mark < 0 || paf->i_pg_one + i_write_end > paf->i_mark ){
    int rc = __apnd_write_mark(paf, file, i_write_end);
    if( DBSQL_OK!=rc ) return rc;
  }
  return file->methods->x_write(file, buf, i_amt, paf->i_pg_one+i_ofst);
}

/*
** Truncate an apnd-file.
*/
static int __apnd_truncate(dbsql_file_t *file, int64_t size){
  apnd_file_t *paf = (apnd_file_t *)file;
  file = ORIGFILE(file);
  /* The append mark goes out first so truncate failure does not lose it. */
  if( DBSQL_OK!=__apnd_write_mark(paf, file, size) ) return DBSQL_IOERR;
  /* Truncate underlying file just past append mark */
  return file->methods->truncate(file, paf->i_mark+APND_MARK_SIZE);
}

/*
** Sync an apnd-file.
*/
static int __apnd_sync(dbsql_file_t *file, int flags){
  file = ORIGFILE(file);
  return file->methods->x_sync(file, flags);
}

/*
** Return the current file-size of an apnd-file.
** If the append mark is not yet there, the file-size is 0.
*/
static int __apnd_file_size(dbsql_file_t *file, int64_t *size){
  apnd_file_t *paf = (apnd_file_t *)file;
  *size = ( paf->i_mark >= 0 )? (paf->i_mark - paf->i_pg_one) : 0;
  return DBSQL_OK;
}

/*
** Lock an apnd-file.
*/
static int __apnd_lock(dbsql_file_t *file, int lock){
  file = ORIGFILE(file);
  return file->methods->lock(file, lock);
}

/*
** Unlock an apnd-file.
*/
static int __apnd_unlock(dbsql_file_t *file, int lock){
  file = ORIGFILE(file);
  return file->methods->unlock(file, lock);
}

/*
** Check if another file-handle holds a RESERVED lock on an apnd-file.
*/
static int __apnd_check_reserved_lock(dbsql_file_t *file, int *res_out){
  file = ORIGFILE(file);
  return file->methods->check_reserved_lock(file, res_out);
}

/*
** File control method. For custom operations on an apnd-file.
*/
static int __apnd_file_control(dbsql_file_t *file, int op, void *arg){
  apnd_file_t *paf = (apnd_file_t *)file;
  int rc;
  file = ORIGFILE(file);
  if( op==DBSQL_FCNTL_SIZE_HINT ) *(int64_t*)arg += paf->i_pg_one;
  rc = file->methods->file_control(file, op, arg);
  if( rc==DBSQL_OK && op==DBSQL_FCNTL_VFSNAME ){
    *(char**)arg = dbsql_mprintf("apnd(%lld)/%z", paf->i_pg_one,*(char**)arg);
  }
  return rc;
}

/*
** Return the sector-size in bytes for an apnd-file.
*/
static int __apnd_sector_size(dbsql_file_t *file){
  file = ORIGFILE(file);
  return file->methods->sector_size(file);
}

/*
** Return the device characteristic flags supported by an apnd-file.
*/
static int __apnd_device_characteristics(dbsql_file_t *file){
  file = ORIGFILE(file);
  return file->methods->device_characteristics(file);
}

/* Create a shared memory file mapping */
static int __apnd_shm_map(
  dbsql_file_t *file,
  int i_pg,
  int pgsz,
  int extend,
  void volatile **pp
){
  file = ORIGFILE(file);
  return file->methods->shm_map(file,i_pg,pgsz,extend,pp);
}

/* Perform locking on a shared-memory segment */
static int __apnd_shm_lock(dbsql_file_t *file, int offset, int n, int flags){
  file = ORIGFILE(file);
  return file->methods->shm_lock(file,offset,n,flags);
}

/* Memory barrier operation on shared memory */
static void __apnd_shm_barrier(dbsql_file_t *file){
  file = ORIGFILE(file);
  file->methods->shm_barrier(file);
}

/* Unmap a shared memory segment */
static int __apnd_shm_unmap(dbsql_file_t *file, int delete_flag){
  file = ORIGFILE(file);
  return file->methods->shm_unmap(file,delete_flag);
}

/* Fetch a page of a memory-mapped file */
static int __apnd_fetch(
  dbsql_file_t *file,
  int64_t i_ofst,
  int i_amt,
  void **pp
){
  apnd_file_t *p = (apnd_file_t *)file;
  if( p->i_mark < 0 || i_ofst+i_amt > p->i_mark ){
    return DBSQL_IOERR; /* Cannot read what is not yet there. */
  }
  file = ORIGFILE(file);
  return file->methods->fetch(file, i_ofst+p->i_pg_one, i_amt, pp);
}

/* Release a memory-mapped page */
static int __apnd_unfetch(dbsql_file_t *file, int64_t i_ofst, void *page){
  apnd_file_t *p = (apnd_file_t *)file;
  file = ORIGFILE(file);
  return file->methods->unfetch(file, i_ofst+p->i_pg_one, page);
}

/*
** Try to read the append-mark off the end of a file.  Return the
** start of the appended database if the append-mark is present.
** If there is no valid append-mark, return -1;
**
** An append-mark is only valid if the NNNNNNNN start-of-database offset
** indicates that the appended database contains at least one page.  The
** start-of-database value must be a multiple of 512.
*/
static int64_t __apnd_read_mark(int64_t sz, dbsql_file_t *file){
  int rc, i;
  int64_t i_mark;
  int msbs = 8 * (APND_MARK_FOS_SZ-1);
  unsigned char a[APND_MARK_SIZE];

  if( APND_MARK_SIZE!=(sz & 0x1ff) ) return -1;
  rc = file->methods->x_read(file, a, APND_MARK_SIZE, sz-APND_MARK_SIZE);
  if( rc ) return -1;
  if( memcmp(a, APND_MARK_PREFIX, APND_MARK_PREFIX_SZ)!=0 ) return -1;
  i_mark = ((int64_t)(a[APND_MARK_PREFIX_SZ] & 0x7f)) << msbs;
  for(i=1; i<8; i++){
    msbs -= 8;
    i_mark |= (int64_t)a[APND_MARK_PREFIX_SZ+i]<<msbs;
  }
  if( i_mark > (sz - APND_MARK_SIZE - 512) ) return -1;
  if( i_mark & 0x1ff ) return -1;
  return i_mark;
}

static const char apvfsdbsql_hdr[] = "SQLite format 3";
/*
** Check to see if the file is an appendvfs dbsql database file.
** Return true iff it is such. Parameter sz is the file's size.
*/
static int __apnd_is_appendvfs_database(int64_t sz, dbsql_file_t *file){
  int rc;
  char hdr[16];
  int64_t i_mark = __apnd_read_mark(sz, file);
  if( i_mark>=0 ){
    /* If file has the correct end-marker, the expected odd size, and the
    ** dbsql DB type marker where the end-marker puts it, then it
    ** is an appendvfs database.
    */
    rc = file->methods->x_read(file, hdr, sizeof(hdr), i_mark);
    if( DBSQL_OK==rc
     && memcmp(hdr, apvfsdbsql_hdr, sizeof(hdr))==0
     && (sz & 0x1ff) == APND_MARK_SIZE
     && sz>=512+APND_MARK_SIZE
    ){
      return 1; /* It's an appendvfs database */
    }
  }
  return 0;
}

/*
** Check to see if the file is an ordinary dbsql database file.
** Return true iff so. Parameter sz is the file's size.
*/
static int __apnd_is_ordinary_database_file(int64_t sz, dbsql_file_t *file){
  char hdr[16];
  if( __apnd_is_appendvfs_database(sz, file) /* rule 2 */
   || (sz & 0x1ff) != 0
   || DBSQL_OK!=file->methods->x_read(file, hdr, sizeof(hdr), 0)
   || memcmp(hdr, apvfsdbsql_hdr, sizeof(hdr))!=0
  ){
    return 0;
  }else{
    return 1;
  }
}

/*
** Open an apnd file handle.
*/
static int __apnd_open(
  dbsql_vfs_t *p_apnd_vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  apnd_file_t *apnd_file = (apnd_file_t*)file;
  dbsql_file_t *base_file = ORIGFILE(file);
  dbsql_vfs_t *base_vfs = ORIGVFS(p_apnd_vfs);
  int rc;
  int64_t sz = 0;
  if( (flags & DBSQL_OPEN_MAIN_DB)==0 ){
    /* The appendvfs is not to be used for transient or temporary databases.
    ** Just use the base VFS open to initialize the given file object and
    ** open the underlying file. (Appendvfs is then unused for this file.)
    */
    return base_vfs->x_open(base_vfs, name, file, flags, out_flags);
  }
  memset(apnd_file, 0, sizeof(apnd_file_t));
  file->methods = &apnd_io_methods;
  apnd_file->i_mark = -1;    /* Append mark not yet written */

  rc = base_vfs->x_open(base_vfs, name, base_file, flags, out_flags);
  if( rc==DBSQL_OK ){
    rc = base_file->methods->file_size(base_file, &sz);
    if( rc ){
      base_file->methods->x_close(base_file);
    }
  }
  if( rc ){
    file->methods = 0;
    return rc;
  }
  if( __apnd_is_ordinary_database_file(sz, base_file) ){
    /* The file being opened appears to be just an ordinary DB. Copy
    ** the base dispatch-table so this instance mimics the base VFS. 
    */
    memmove(apnd_file, base_file, base_vfs->sz_os_file);
    return DBSQL_OK;
  }
  apnd_file->i_pg_one = __apnd_read_mark(sz, file);
  if( apnd_file->i_pg_one>=0 ){
    apnd_file->i_mark = sz - APND_MARK_SIZE; /* Append mark found */
    return DBSQL_OK;
  }
  if( (flags & DBSQL_OPEN_CREATE)==0 ){
    base_file->methods->x_close(base_file);
    rc = DBSQL_CANTOPEN;
    file->methods = 0;
  }else{
    /* Round newly added appendvfs location to #define'd page boundary. 
    ** Note that nothing has yet been written to the underlying file.
    ** The append mark will be written along with first content write.
    ** Until then, paf->i_mark value indicates it is not yet written.
    */
    apnd_file->i_pg_one = APND_START_ROUNDUP(sz);
  }
  return rc;
}

/*
** Delete an apnd file.
** For an appendvfs, this could mean delete the appendvfs portion,
** leaving the appendee as it was before it gained an appendvfs.
** For now, this code deletes the underlying file too.
*/
static int __apnd_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  return ORIGVFS(vfs)->x_delete(ORIGVFS(vfs), path, dir_sync);
}

/*
** All other VFS methods are pass-thrus.
*/
static int __apnd_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  return ORIGVFS(vfs)->x_access(ORIGVFS(vfs), path, flags, res_out);
}
static int __apnd_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  return ORIGVFS(vfs)->full_pathname(ORIGVFS(vfs),path,n_out,out);
}
static void *__apnd_dl_open(dbsql_vfs_t *vfs, const char *path){
  return ORIGVFS(vfs)->dl_open(ORIGVFS(vfs), path);
}
static void __apnd_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  ORIGVFS(vfs)->dl_error(ORIGVFS(vfs), n_byte, errmsg);
}
static void (*apnd_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return ORIGVFS(vfs)->dl_sym(ORIGVFS(vfs), p, sym);
}
static void __apnd_dl_close(dbsql_vfs_t *vfs, void *handle){
  ORIGVFS(vfs)->dl_close(ORIGVFS(vfs), handle);
}
static int __apnd_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return ORIGVFS(vfs)->randomness(ORIGVFS(vfs), n_byte, buf_out);
}
static int __apnd_sleep(dbsql_vfs_t *vfs, int n_micro){
  return ORIGVFS(vfs)->x_sleep(ORIGVFS(vfs), n_micro);
}
static int __apnd_current_time(dbsql_vfs_t *vfs, double *time_out){
  return ORIGVFS(vfs)->current_time(ORIGVFS(vfs), time_out);
}
static int __apnd_get_last_error(dbsql_vfs_t *vfs, int a, char *b){
  return ORIGVFS(vfs)->get_last_error(ORIGVFS(vfs), a, b);
}
static int __apnd_current_time_int64(dbsql_vfs_t *vfs, int64_t *p){
  return ORIGVFS(vfs)->current_time_int64(ORIGVFS(vfs), p);
}
static int __apnd_set_system_call(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_syscall_ptr call
){
  return ORIGVFS(vfs)->set_system_call(ORIGVFS(vfs),name,call);
}
static dbsql_syscall_ptr __apnd_get_system_call(
  dbsql_vfs_t *vfs,
  const char *name
){
  return ORIGVFS(vfs)->get_system_call(ORIGVFS(vfs),name);
}
static const char *__apnd_next_system_call(dbsql_vfs_t *vfs, const char *name){
  return ORIGVFS(vfs)->next_system_call(ORIGVFS(vfs), name);
}

  
#ifdef _WIN32
__declspec(dllexport)
#endif
/* 
** This routine is called when the extension is loaded.
** Register the new VFS.
*/
int dbsql_appendvfs_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  dbsql_vfs_t *orig;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;
  (void)db;
  orig = dbsql_vfs_find(0);
  if( orig==0 ) return DBSQL_ERROR;
  apnd_vfs.i_version = orig->i_version;
  apnd_vfs.app_data = orig;
  apnd_vfs.sz_os_file = orig->sz_os_file + sizeof(apnd_file_t);
  rc = dbsql_vfs_register(&apnd_vfs, 0);
#ifdef APPENDVFS_TEST
  if( rc==DBSQL_OK ){
    rc = dbsql_auto_extension((void(*)(void))apndvfs_register);
  }
#endif
  if( rc==DBSQL_OK ) rc = DBSQL_OK_LOAD_PERMANENTLY;
  return rc;
}
