/*
** 2020-04-20
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
** This file implements a VFS shim that writes a checksum on each page
** of an dbsql database file.  When reading pages, the checksum is verified
** and an error is raised if the checksum is incorrect.
**
** COMPILING
**
** This extension requires dbsql 3.32.0 or later.  It uses the
** dbsql_database_file_object() interface which was added in
** version 3.32.0, so it will not link with an earlier version of
** dbsql.
**
** To build this extension as a separately loaded shared library or
** DLL, use compiler command-lines similar to the following:
**
**   (linux)    gcc -f_pic -shared cksumvfs.c -o cksumvfs.so
**   (mac)      clang -f_pic -dynamiclib cksumvfs.c -o cksumvfs.dylib
**   (windows)  cl cksumvfs.c -link -dll -out:cksumvfs.dll
**
** You may want to add additional compiler options, of course,
** according to the needs of your project.
**
** If you want to statically link this extension with your product,
** then compile it like any other C-language module but add the
** "-DSQLITE_CKSUMVFS_STATIC" option so that this module knows that
** it is being statically linked rather than dynamically linked
**
** LOADING
**
** To load this extension as a shared library, you first have to
** bring up a dummy dbsql database connection to use as the argument
** to the dbsql_load_extension() API call.  Then you invoke the
** dbsql_load_extension() API and shutdown the dummy database
** connection.  All subsequent database connections that are opened
** will include this extension.  For example:
**
**     DBSQL *db;
**     dbsql_open(":memory:", &db);
**     dbsql_load_extension(db, "./cksumvfs");
**     dbsql_close(db);
**
** If this extension is compiled with -DSQLITE_CKSUMVFS_STATIC and
** statically linked against the application, initialize it using
** a single API call as follows:
**
**     dbsql_register_cksumvfs();
**
** Cksumvfs is a VFS Shim. When loaded, "cksmvfs" becomes the new
** default VFS and it uses the prior default VFS as the next VFS
** down in the stack.  This is normally what you want.  However, in
** complex situations where multiple VFS shims are being loaded,
** it might be important to ensure that cksumvfs is loaded in the
** correct order so that it sequences itself into the default VFS
** Shim stack in the right order.
**
** USING
**
** Open database connections using the dbsql_open() or 
** dbsql_open_v2() interfaces, as normal.  Ordinary database files
** (without a checksum) will operate normally.  Databases with 
** checksums will return an DBSQL_IOERR_DATA error if a page is
** encountered that contains an invalid checksum.
**
** Checksumming only works on databases that have a reserve-bytes
** value of exactly 8.  The default value for reserve-bytes is 0.
** Hence, newly created database files will omit the checksum by
** default.  To create a database that includes a checksum, change
** the reserve-bytes value to 8 by runing:
**
**    int n = 8;
**    dbsql_file_control(db, 0, DBSQL_FCNTL_RESERVE_BYTES, &n);
**
** If you do this immediately after creating a new database file,
** before anything else has been written into the file, then that
** might be all that you need to do.  Otherwise, the API call
** above should be followed by:
**
**    dbsql_exec(db, "VACUUM", 0, 0, 0);
**
** It never hurts to run the VACUUM, even if you don't need it.
** If the database is in WAL mode, you should shutdown and
** reopen all database connections before continuing.
**
** From the CLI, use the ".filectrl reserve_bytes 8" command, 
** followed by "VACUUM;".
**
** Note that dbsql allows the number of reserve-bytes to be
** increased but not decreased.  So if a database file already
** has a reserve-bytes value greater than 8, there is no way to
** activate checksumming on that database, other than to dump
** and restore the database file.  Note also that other extensions
** might also make use of the reserve-bytes.  Checksumming will
** be incompatible with those other extensions.
**
** VERIFICATION OF CHECKSUMS
**
** If any checksum is incorrect, the "PRAGMA quick_check" command
** will find it.  To verify that checksums are actually enabled
** and running, use the following query:
**
**   SELECT count(*), verify_checksum(data)
**     FROM dbsql_dbpage
**    GROUP BY 2;
**
** There are three possible outputs form the verify_checksum()
** function: 1, 0, and NULL.  1 is returned if the checksum is
** correct.  0 is returned if the checksum is incorrect.  NULL
** is returned if the page is unreadable.  If checksumming is
** enabled, the read will fail if the checksum is wrong, so the
** usual result from verify_checksum() on a bad checksum is NULL.
**
** If everything is OK, the query above should return a single
** row where the second column is 1.  Any other result indicates
** either that there is a checksum error, or checksum validation
** is disabled.
**
** CONTROLLING CHECKSUM VERIFICATION
**
** The cksumvfs extension implements a new PRAGMA statement that can
** be used to disable, re-enable, or query the status of checksum
** verification:
**
**    PRAGMA checksum_verification;          -- query status
**    PRAGMA checksum_verification=OFF;      -- disable verification
**    PRAGMA checksum_verification=ON;       -- re-enable verification
**
** The "checksum_verification" pragma will return "1" (true) or "0"
** (false) if checksum verification is enabled or disabled, respectively.
** "Verification" in this context means the feature that causes
** DBSQL_IOERR_DATA errors if a checksum mismatch is detected while
** reading.  Checksums are always kept up-to-date as long as the
** reserve-bytes value of the database is 8, regardless of the setting
** of this pragma.  Checksum verification can be disabled (for example)
** to do forensic analysis of a database that has previously reported
** a checksum error.
**
** The "checksum_verification" pragma will always respond with "0" if
** the database file does not have a reserve-bytes value of 8.  The
** pragma will return no rows at all if the cksumvfs extension is
** not loaded.
**
** IMPLEMENTATION NOTES
**
** The checksum is stored in the last 8 bytes of each page.  This
** module only operates if the "bytes of reserved space on each page"
** value at offset 20 the dbsql database header is exactly 8.  If
** the reserved-space value is not 8, this module is a no-op.
*/
#if defined(DBSQL_AMALGAMATION) && !defined(DBSQL_CKSUMVFS_STATIC)
# define DBSQL_CKSUMVFS_STATIC
#endif
#ifdef DBSQL_CKSUMVFS_STATIC
# include "dbsql.h"
#else
# include "dbsql_ext.h"
  DBSQL_EXTENSION_INIT1
#endif
#include <string.h>
#include <assert.h>


/*
** Forward declaration of objects used by this utility
*/
typedef struct dbsql_vfs_t cksm_vfs_t;
typedef struct cksm_file_t cksm_file_t;

/*
** Useful datatype abbreviations
*/
#if !defined(DBSQL_AMALGAMATION)
  typedef unsigned char u_int8_t;
  typedef unsigned int u_int32_t;
#endif

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p)  ((dbsql_vfs_t*)((p)->app_data))
#define ORIGFILE(p) ((dbsql_file_t*)(((cksm_file_t*)(p))+1))

/* An open file */
struct cksm_file_t {
  dbsql_file_t base;    /* IO methods */
  const char *f_name;   /* Original name of the file */
  char compute_cksm;     /* True to compute checksums.
                        ** Always true if reserve size is 8. */
  char verify_cksm;      /* True to verify checksums */
  cksm_file_t *partner;   /* Ptr from WAL to main-db, or from main-db to WAL */
};

/*
** Methods for cksm_file_t
*/
static int __cksm_close(dbsql_file_t*);
static int __cksm_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __cksm_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __cksm_truncate(dbsql_file_t*, int64_t size);
static int __cksm_sync(dbsql_file_t*, int flags);
static int __cksm_file_size(dbsql_file_t*, int64_t *size);
static int __cksm_lock(dbsql_file_t*, int);
static int __cksm_unlock(dbsql_file_t*, int);
static int __cksm_check_reserved_lock(dbsql_file_t*, int *res_out);
static int __cksm_file_control(dbsql_file_t*, int op, void *arg);
static int __cksm_sector_size(dbsql_file_t*);
static int __cksm_device_characteristics(dbsql_file_t*);
static int __cksm_shm_map(dbsql_file_t*, int i_pg, int pgsz, int, void volatile**);
static int __cksm_shm_lock(dbsql_file_t*, int offset, int n, int flags);
static void __cksm_shm_barrier(dbsql_file_t*);
static int __cksm_shm_unmap(dbsql_file_t*, int delete_flag);
static int __cksm_fetch(dbsql_file_t*, int64_t i_ofst, int i_amt, void **pp);
static int __cksm_unfetch(dbsql_file_t*, int64_t i_ofst, void *p);

/*
** Methods for cksm_vfs_t
*/
static int __cksm_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __cksm_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __cksm_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __cksm_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
static void *__cksm_dl_open(dbsql_vfs_t*, const char *filename);
static void __cksm_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*cksm_dl_sym(dbsql_vfs_t *vfs, void *p, const char*sym))(void);
static void __cksm_dl_close(dbsql_vfs_t*, void*);
static int __cksm_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __cksm_sleep(dbsql_vfs_t*, int microseconds);
static int __cksm_current_time(dbsql_vfs_t*, double*);
static int __cksm_get_last_error(dbsql_vfs_t*, int, char *);
static int __cksm_current_time_int64(dbsql_vfs_t*, int64_t*);
static int __cksm_set_system_call(dbsql_vfs_t*, const char*,dbsql_syscall_ptr);
static dbsql_syscall_ptr __cksm_get_system_call(dbsql_vfs_t*, const char *z);
static const char *__cksm_next_system_call(dbsql_vfs_t*, const char *name);

static dbsql_vfs_t cksm_vfs = {
  3,                            /* i_version (set when registered) */
  0,                            /* sz_os_file (set when registered) */
  1024,                         /* mx_pathname */
  0,                            /* next */
  "cksmvfs",                    /* name */
  0,                            /* app_data (set when registered) */ 
  __cksm_open,                     /* x_open */
  __cksm_delete,                   /* x_delete */
  __cksm_access,                   /* x_access */
  __cksm_full_pathname,             /* full_pathname */
  __cksm_dl_open,                   /* dl_open */
  __cksm_dl_error,                  /* dl_error */
  cksm_dl_sym,                    /* dl_sym */
  __cksm_dl_close,                  /* dl_close */
  __cksm_randomness,               /* randomness */
  __cksm_sleep,                    /* x_sleep */
  __cksm_current_time,              /* current_time */
  __cksm_get_last_error,             /* get_last_error */
  __cksm_current_time_int64,         /* current_time_int64 */
  __cksm_set_system_call,            /* set_system_call */
  __cksm_get_system_call,            /* get_system_call */
  __cksm_next_system_call            /* next_system_call */
};

static const dbsql_io_methods_t cksm_io_methods = {
  3,                              /* i_version */
  __cksm_close,                      /* x_close */
  __cksm_read,                       /* x_read */
  __cksm_write,                      /* x_write */
  __cksm_truncate,                   /* truncate */
  __cksm_sync,                       /* x_sync */
  __cksm_file_size,                   /* file_size */
  __cksm_lock,                       /* lock */
  __cksm_unlock,                     /* unlock */
  __cksm_check_reserved_lock,          /* check_reserved_lock */
  __cksm_file_control,                /* file_control */
  __cksm_sector_size,                 /* sector_size */
  __cksm_device_characteristics,      /* device_characteristics */
  __cksm_shm_map,                     /* shm_map */
  __cksm_shm_lock,                    /* shm_lock */
  __cksm_shm_barrier,                 /* shm_barrier */
  __cksm_shm_unmap,                   /* shm_unmap */
  __cksm_fetch,                      /* fetch */
  __cksm_unfetch                     /* unfetch */
};

/* Do byte swapping on a unsigned 32-bit integer */
#define BYTESWAP32(x) ( \
    (((x)&0x000000FF)<<24) + (((x)&0x0000FF00)<<8)  \
  + (((x)&0x00FF0000)>>8)  + (((x)&0xFF000000)>>24) \
)

/* Compute a checksum on a buffer */
static void __cksm_compute(
  u_int8_t *a,           /* Content to be checksummed */
  int n_byte,       /* Bytes of content in a[].  Must be a multiple of 8. */
  u_int8_t *outs         /* OUT: Final 8-byte checksum value output */
){
  u_int32_t s1 = 0, s2 = 0;
  u_int32_t *data = (u_int32_t*)a;
  u_int32_t *ends = (u_int32_t*)&a[n_byte];
  u_int32_t x = 1;

  assert( n_byte>=8 );
  assert( (n_byte&0x00000007)==0 );
  assert( n_byte<=65536 );

  if( 1 == *(u_int8_t*)&x ){
    /* Little-endian */
    do {
      s1 += *data++ + s2;
      s2 += *data++ + s1;
    }while( data<ends );
  }else{
    /* Big-endian */
    do {
      s1 += BYTESWAP32(data[0]) + s2;
      s2 += BYTESWAP32(data[1]) + s1;
      data += 2;
    }while( data<ends );
    s1 = BYTESWAP32(s1);
    s2 = BYTESWAP32(s2);
  }
  memcpy(outs, &s1, 4);
  memcpy(outs+4, &s2, 4);
}

/*
** SQL function:    verify_checksum(BLOB)
**
** Return 0 or 1 if the checksum is invalid or valid.  Or return
** NULL if the input is not a BLOB that is the right size for a
** database page.
*/
static void __cksm_verify_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int n_byte;
  u_int8_t *data;
  u_int8_t cksum[8];
  data = (u_int8_t*)dbsql_value_blob(argv[0]);
  if( data==0 ) return;
  if( dbsql_value_type(argv[0])!=DBSQL_BLOB ) return;
  n_byte = dbsql_value_bytes(argv[0]);
  if( n_byte<512 || n_byte>65536 || (n_byte & (n_byte-1))!=0 ) return;
  __cksm_compute(data, n_byte-8, cksum);
  dbsql_result_int(context, memcmp(data+n_byte-8,cksum,8)==0);
}

#ifdef DBSQL_CKSUMVFS_INIT_FUNCNAME
/*
** SQL function:    initialize_cksumvfs(SCHEMANAME)
**
** This SQL functions (whose name is actually determined at compile-time
** by the value of the DBSQL_CKSUMVFS_INIT_FUNCNAME macro) invokes:
**
**   dbsql_file_control(db, SCHEMANAME, DBSQL_FCNTL_RESERVE_BYTE, &n);
**
** In order to set the reserve bytes value to 8, so that cksumvfs will
** operation.  This feature is provided (if and only if the
** DBSQL_CKSUMVFS_INIT_FUNCNAME compile-time option is set to a string
** which is the name of the SQL function) so as to provide the ability
** to invoke the file-control in programming languages that lack
** direct access to the dbsql_file_control() interface (ex: Java).
**
** This interface is undocumented, apart from this comment.  Usage
** example:
**
**    1.  Compile with -DSQLITE_CKSUMVFS_INIT_FUNCNAME="ckvfs_init"
**    2.  Run:  "SELECT cksum_init('main'); VACUUM;"
*/
static void __cksm_init_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int n_byte = 8;
  const char *schema_name = (const char*)dbsql_value_text(argv[0]);
  DBSQL *db = dbsql_context_db_handle(context);
  dbsql_file_control(db, schema_name, DBSQL_FCNTL_RESERVE_BYTES, &n_byte);
  /* Return NULL */
}
#endif /* DBSQL_CKSUMBFS_INIT_FUNCNAME */

/*
** Close a cksm-file.
*/
static int __cksm_close(dbsql_file_t *file){
  cksm_file_t *p = (cksm_file_t *)file;
  if( p->partner ){
    assert( p->partner->partner==p );
    p->partner->partner = 0;
    p->partner = 0;
  }
  file = ORIGFILE(file);
  return file->methods->x_close(file);
}

/*
** Set the compute_ck_sm and verify_cksm flags, if they need to be
** changed.
*/
static void __cksm_set_flags(cksm_file_t *p, int has_correct_reserve_size){
  if( has_correct_reserve_size!=p->compute_cksm ){
    p->compute_cksm = p->verify_cksm = has_correct_reserve_size;
    if( p->partner ){
      p->partner->verify_cksm = has_correct_reserve_size;
      p->partner->compute_cksm = has_correct_reserve_size;
    }
  }
}

/*
** Read data from a cksm-file.
*/
static int __cksm_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc;
  cksm_file_t *p = (cksm_file_t *)file;
  file = ORIGFILE(file);
  rc = file->methods->x_read(file, buf, i_amt, i_ofst);
  if( rc==DBSQL_OK ){
    if( i_ofst==0 && i_amt>=100 && (
          memcmp(buf,"SQLite format 3",16)==0 || memcmp(buf,"ZV-",3)==0 
    )){
      u_int8_t *d = (u_int8_t*)buf;
      char has_correct_reserve_size = (d[20]==8);
      __cksm_set_flags(p, has_correct_reserve_size);
    }
    /* Verify the checksum if
    **    (1) the size indicates that we are dealing with a complete
    **        database page
    **    (2) checksum verification is enabled
    */
    if( i_amt>=512 && (i_amt & (i_amt-1))==0   /* (1) */
     && p->verify_cksm                       /* (2) */
    ){
      u_int8_t cksum[8];
      __cksm_compute((u_int8_t*)buf, i_amt-8, cksum);
      if( memcmp((u_int8_t*)buf+i_amt-8, cksum, 8)!=0 ){
        dbsql_log(DBSQL_IOERR_DATA,
           "checksum fault offset %lld of \"%s\"",
           i_ofst, p->f_name);
        rc = DBSQL_IOERR_DATA;
      }
    }
  }
  return rc;
}

/*
** Write data to a cksm-file.
*/
static int __cksm_write(
  dbsql_file_t *file,
  const void *buf,
  int i_amt,
  int64_t i_ofst
){
  cksm_file_t *p = (cksm_file_t *)file;
  file = ORIGFILE(file);
  if( i_ofst==0 && i_amt>=100 && (
        memcmp(buf,"SQLite format 3",16)==0 || memcmp(buf,"ZV-",3)==0 
  )){
    u_int8_t *d = (u_int8_t*)buf;
    char has_correct_reserve_size = (d[20]==8);
    __cksm_set_flags(p, has_correct_reserve_size);
  }
  /* If the write size is appropriate for a database page and if
  ** checksums where ever enabled, then it will be safe to compute
  ** the checksums.  The reserve byte size might have increased, but
  ** it will never decrease.  And because it cannot decrease, the
  ** checksum will not overwrite anything.
  */
  if( i_amt>=512
   && p->compute_cksm
  ){
    __cksm_compute((u_int8_t*)buf, i_amt-8, ((u_int8_t*)buf)+i_amt-8);
  }
  return file->methods->x_write(file, buf, i_amt, i_ofst);
}

/*
** Truncate a cksm-file.
*/
static int __cksm_truncate(dbsql_file_t *file, int64_t size){
  file = ORIGFILE(file);
  return file->methods->truncate(file, size);
}

/*
** Sync a cksm-file.
*/
static int __cksm_sync(dbsql_file_t *file, int flags){
  file = ORIGFILE(file);
  return file->methods->x_sync(file, flags);
}

/*
** Return the current file-size of a cksm-file.
*/
static int __cksm_file_size(dbsql_file_t *file, int64_t *size){
  cksm_file_t *p = (cksm_file_t *)file;
  file = ORIGFILE(p);
  return file->methods->file_size(file, size);
}

/*
** Lock a cksm-file.
*/
static int __cksm_lock(dbsql_file_t *file, int lock){
  file = ORIGFILE(file);
  return file->methods->lock(file, lock);
}

/*
** Unlock a cksm-file.
*/
static int __cksm_unlock(dbsql_file_t *file, int lock){
  file = ORIGFILE(file);
  return file->methods->unlock(file, lock);
}

/*
** Check if another file-handle holds a RESERVED lock on a cksm-file.
*/
static int __cksm_check_reserved_lock(dbsql_file_t *file, int *res_out){
  file = ORIGFILE(file);
  return file->methods->check_reserved_lock(file, res_out);
}

/*
** File control method. For custom operations on a cksm-file.
*/
static int __cksm_file_control(dbsql_file_t *file, int op, void *arg){
  int rc;
  cksm_file_t *p = (cksm_file_t*)file;
  file = ORIGFILE(file);
  if( op==DBSQL_FCNTL_PRAGMA ){
    char **args = (char**)arg;
    assert( args[1]!=0 );
    if( dbsql_stricmp(args[1],"checksum_verification")==0 ){
      char *z_arg = args[2];
      if( z_arg!=0 ){
        if( (z_arg[0]>='1' && z_arg[0]<='9')
         || dbsql_strlike("enable%",z_arg,0)==0
         || dbsql_stricmp("yes",z_arg)==0
         || dbsql_stricmp("on",z_arg)==0
        ){
          p->verify_cksm = p->compute_cksm;
        }else{
          p->verify_cksm = 0;
        }
        if( p->partner ) p->partner->verify_cksm = p->verify_cksm;
      }
      args[0] = dbsql_mprintf("%d",p->verify_cksm);
      return DBSQL_OK;
    }else if( p->compute_cksm && args[2]!=0
           && dbsql_stricmp(args[1], "page_size")==0 ){
      /* Do not allow page size changes on a checksum database */
      return DBSQL_OK;
    }
  }
  rc = file->methods->file_control(file, op, arg);
  if( rc==DBSQL_OK && op==DBSQL_FCNTL_VFSNAME ){
    *(char**)arg = dbsql_mprintf("cksm/%z", *(char**)arg);
  }
  return rc;
}

/*
** Return the sector-size in bytes for a cksm-file.
*/
static int __cksm_sector_size(dbsql_file_t *file){
  file = ORIGFILE(file);
  return file->methods->sector_size(file);
}

/*
** Return the device characteristic flags supported by a cksm-file.
*/
static int __cksm_device_characteristics(dbsql_file_t *file){
  int devchar = 0;
  file = ORIGFILE(file);
  devchar = file->methods->device_characteristics(file);
  return (devchar & ~DBSQL_IOCAP_SUBPAGE_READ);
}

/* Create a shared memory file mapping */
static int __cksm_shm_map(
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
static int __cksm_shm_lock(dbsql_file_t *file, int offset, int n, int flags){
  file = ORIGFILE(file);
  return file->methods->shm_lock(file,offset,n,flags);
}

/* Memory barrier operation on shared memory */
static void __cksm_shm_barrier(dbsql_file_t *file){
  file = ORIGFILE(file);
  file->methods->shm_barrier(file);
}

/* Unmap a shared memory segment */
static int __cksm_shm_unmap(dbsql_file_t *file, int delete_flag){
  file = ORIGFILE(file);
  return file->methods->shm_unmap(file,delete_flag);
}

/* Fetch a page of a memory-mapped file */
static int __cksm_fetch(
  dbsql_file_t *file,
  int64_t i_ofst,
  int i_amt,
  void **pp
){
  cksm_file_t *p = (cksm_file_t *)file;
  if( p->compute_cksm ){
    *pp = 0;
    return DBSQL_OK;
  }
  file = ORIGFILE(file);
  if( file->methods->i_version>2 && file->methods->fetch ){
    return file->methods->fetch(file, i_ofst, i_amt, pp);
  }
  *pp = 0;
  return DBSQL_OK;
}

/* Release a memory-mapped page */
static int __cksm_unfetch(dbsql_file_t *file, int64_t i_ofst, void *page){
  file = ORIGFILE(file);
  if( file->methods->i_version>2 && file->methods->unfetch ){
    return file->methods->unfetch(file, i_ofst, page);
  }
  return DBSQL_OK;
}

/*
** Open a cksm file handle.
*/
static int __cksm_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  cksm_file_t *p;
  dbsql_file_t *sub_file;
  dbsql_vfs_t *sub_vfs;
  int rc;
  sub_vfs = ORIGVFS(vfs);
  if( (flags & DBSQL_OPEN_MAIN_DB)==0 ){
    return sub_vfs->x_open(sub_vfs, name, file, flags, out_flags);
  }
  p = (cksm_file_t*)file;
  memset(p, 0, sizeof(*p));
  sub_file = ORIGFILE(file);
  file->methods = &cksm_io_methods;
  rc = sub_vfs->x_open(sub_vfs, name, sub_file, flags, out_flags);
  if( rc ) goto cksm_open_done;
  p->f_name = name;
cksm_open_done:
  if( rc ) file->methods = 0;
  return rc;
}

/*
** All other VFS methods are pass-thrus.
*/
static int __cksm_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  return ORIGVFS(vfs)->x_delete(ORIGVFS(vfs), path, dir_sync);
}
static int __cksm_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  return ORIGVFS(vfs)->x_access(ORIGVFS(vfs), path, flags, res_out);
}
static int __cksm_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  return ORIGVFS(vfs)->full_pathname(ORIGVFS(vfs),path,n_out,out);
}
static void *__cksm_dl_open(dbsql_vfs_t *vfs, const char *path){
  return ORIGVFS(vfs)->dl_open(ORIGVFS(vfs), path);
}
static void __cksm_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  ORIGVFS(vfs)->dl_error(ORIGVFS(vfs), n_byte, errmsg);
}
static void (*cksm_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return ORIGVFS(vfs)->dl_sym(ORIGVFS(vfs), p, sym);
}
static void __cksm_dl_close(dbsql_vfs_t *vfs, void *handle){
  ORIGVFS(vfs)->dl_close(ORIGVFS(vfs), handle);
}
static int __cksm_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return ORIGVFS(vfs)->randomness(ORIGVFS(vfs), n_byte, buf_out);
}
static int __cksm_sleep(dbsql_vfs_t *vfs, int n_micro){
  return ORIGVFS(vfs)->x_sleep(ORIGVFS(vfs), n_micro);
}
static int __cksm_current_time(dbsql_vfs_t *vfs, double *time_out){
  return ORIGVFS(vfs)->current_time(ORIGVFS(vfs), time_out);
}
static int __cksm_get_last_error(dbsql_vfs_t *vfs, int a, char *b){
  return ORIGVFS(vfs)->get_last_error(ORIGVFS(vfs), a, b);
}
static int __cksm_current_time_int64(dbsql_vfs_t *vfs, int64_t *p){
  dbsql_vfs_t *orig = ORIGVFS(vfs);
  int rc;
  assert( orig->i_version>=2 );
  if( orig->current_time_int64 ){
    rc = orig->current_time_int64(orig, p);
  }else{
    double r;
    rc = orig->current_time(orig, &r);
    *p = (int64_t)(r*86400000.0);
  }
  return rc;
}
static int __cksm_set_system_call(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_syscall_ptr call
){
  return ORIGVFS(vfs)->set_system_call(ORIGVFS(vfs),name,call);
}
static dbsql_syscall_ptr __cksm_get_system_call(
  dbsql_vfs_t *vfs,
  const char *name
){
  return ORIGVFS(vfs)->get_system_call(ORIGVFS(vfs),name);
}
static const char *__cksm_next_system_call(dbsql_vfs_t *vfs, const char *name){
  return ORIGVFS(vfs)->next_system_call(ORIGVFS(vfs), name);
}

/* Register the verify_checksum() SQL function.
*/
static int __cksm_register_func(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc;
  if( db==0 ) return DBSQL_OK;
  rc = dbsql_create_function(db, "verify_checksum", 1,
                   DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                   0, __cksm_verify_func, 0, 0);
#ifdef DBSQL_CKSUMVFS_INIT_FUNCNAME
  (void)dbsql_create_function(db, DBSQL_CKSUMVFS_INIT_FUNCNAME, 1,
                   DBSQL_UTF8|DBSQL_DIRECTONLY,
                   0, __cksm_init_func, 0, 0);
#endif
  return rc;
}

/*
** Register the cksum VFS as the default VFS for the system.
** Also make arrangements to automatically register the "verify_checksum()"
** SQL function on each new database connection.
*/
static int __cksm_register_vfs(void){
  int rc = DBSQL_OK;
  dbsql_vfs_t *orig = dbsql_vfs_find(0);
  if( orig==0 ) return DBSQL_ERROR;
  cksm_vfs.i_version = orig->i_version;
  cksm_vfs.app_data = orig;
  cksm_vfs.sz_os_file = orig->sz_os_file + sizeof(cksm_file_t);
  rc = dbsql_vfs_register(&cksm_vfs, 1);
  if( rc==DBSQL_OK ){
    rc = dbsql_auto_extension((void(*)(void))__cksm_register_func);
  }
  return rc;
}

#if defined(DBSQL_CKSUMVFS_STATIC)
/* This variant of the initializer runs when the extension is
** statically linked.
*/
int dbsql_register_cksumvfs(const char *__not_used){
  (void)__not_used;
  return __cksm_register_vfs();
}
int dbsql_unregister_cksumvfs(void){
  if( dbsql_vfs_find("cksmvfs") ){
    dbsql_vfs_unregister(&cksm_vfs);
    dbsql_cancel_auto_extension((void(*)(void))__cksm_register_func);
  }
  return DBSQL_OK;
}
#endif /* defined(DBSQL_CKSUMVFS_STATIC */

#if !defined(DBSQL_CKSUMVFS_STATIC)
/* This variant of the initializer function is used when the
** extension is shared library to be loaded at run-time.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
/* 
** This routine is called by dbsql_load_extension() when the
** extension is first loaded.
***/
int dbsql_cksumvfs_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p; /* not used */
  rc = __cksm_register_func(db, 0, 0);
  if( rc==DBSQL_OK ){
    rc = __cksm_register_vfs();
  }
  if( rc==DBSQL_OK ) rc = DBSQL_OK_LOAD_PERMANENTLY;
  return rc;
}
#endif /* !defined(DBSQL_CKSUMVFS_STATIC) */
