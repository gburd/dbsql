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
** This file contains code that modified the OS layer in order to simulate
** the effect on the database file of an OS crash or power failure.  This
** is used to test the ability of dbsql to recover from those situations.
*/
#if DBSQL_TEST          /* This file is used for testing only */
#include "dbsql_int.h"
#include "tcl_dbsql.h"

#ifndef DBSQL_OMIT_DISKIO  /* This file is a no-op if disk I/O is disabled */

/* #define TRACE_CRASHTEST */

typedef struct crash_file_t crash_file_t;
typedef struct crash_global_t crash_global_t;
typedef struct write_buffer_t write_buffer_t;

/*
** Method:
**
**   This layer is implemented as a wrapper around the "real"
**   dbsql_file_t object for the host system. Each time data is
**   written to the file object, instead of being written to the
**   underlying file, the write operation is stored in an in-memory
**   structure (type write_buffer_t). This structure is placed at the
**   end of a global ordered list (the write-list).
**
**   When data is read from a file object, the requested region is
**   first retrieved from the real file. The write-list is then
**   traversed and data copied from any overlapping write_buffer_t
**   structures to the output buffer. i.e. a read() operation following
**   one or more write() operations works as expected, even if no
**   data has actually been written out to the real file.
**
**   When a fsync() operation is performed, an operating system crash
**   may be simulated, in which case exit(-1) is called (the call to
**   x_sync() never returns). Whether or not a crash is simulated,
**   the data associated with a subset of the write_buffer_t structures
**   stored in the write-list is written to the real underlying files
**   and the entries removed from the write-list. If a crash is simulated,
**   a subset of the buffers may be corrupted before the data is written.
**
**   The exact subset of the write-list written and/or corrupted is
**   determined by the simulated device characteristics and sector-size.
**
** "Normal" mode:
**
**   Normal mode is used when the simulated device has none of the
**   DBSQL_IOCAP_XXX flags set.
**
**   In normal mode, if the fsync() is not a simulated crash, the
**   write-list is traversed from beginning to end. Each write_buffer_t
**   structure associated with the file handle used to call x_sync()
**   is written to the real file and removed from the write-list.
**
**   If a crash is simulated, one of the following takes place for
**   each write_buffer_t in the write-list, regardless of which
**   file-handle it is associated with:
**
**     1. The buffer is correctly written to the file, just as if
**        a crash were not being simulated.
**
**     2. Nothing is done.
**
**     3. Garbage data is written to all sectors of the file that
**        overlap the region specified by the write_buffer_t. Or garbage
**        data is written to some contiguous section within the
**        overlapped sectors.
**
** Device Characteristic flag handling:
**
**   If the IOCAP_ATOMIC flag is set, then option (3) above is
**   never selected.
**
**   If the IOCAP_ATOMIC512 flag is set, and the write_buffer_t represents
**   an aligned write() of an integer number of 512 byte regions, then
**   option (3) above is never selected. Instead, each 512 byte region
**   is either correctly written or left completely untouched. Similar
**   logic governs the behavior if any of the other ATOMICXXX flags
**   is set.
**
**   If either the IOCAP_SAFEAPPEND or IOCAP_SEQUENTIAL flags are set
**   and a crash is being simulated, then an entry of the write-list is
**   selected at random. Everything in the list after the selected entry
**   is discarded before processing begins.
**
**   If IOCAP_SEQUENTIAL is set and a crash is being simulated, option
**   (1) is selected for all write-list entries except the last. If a
**   crash is not being simulated, then all entries in the write-list
**   that occur before at least one write() on the file-handle specified
**   as part of the x_sync() are written to their associated real files.
**
**   If IOCAP_SAFEAPPEND is set and the first byte written by the write()
**   operation is one byte past the current end of the file, then option
**   (1) is always selected.
*/

/*
** Each write operation in the write-list is represented by an instance
** of the following structure.
**
** If buf is 0, then this structure represents a call to truncate(),
** not x_write(). In that case, i_offset is the size that the file is
** truncated to.
*/
struct write_buffer_t {
  int64_t i_offset;                 /* Byte offset of the start of this write() */
  int n_buf;                    /* Number of bytes written */
  u_int8_t *buf;                    /* Pointer to copy of written data */
  crash_file_t *file;            /* File this write() applies to */

  write_buffer_t *next;          /* Next in crash_global_t.write_list */
};

struct crash_file_t {
  const dbsql_io_methods_t *method;   /* Must be first */
  dbsql_file_t *real_file;             /* Underlying "real" file handle */
  char *name;
  int flags;                           /* Flags the file was opened with */

  /* Cache of the entire file. This is used to speed up __os_read() and
  ** __os_file_size() calls. Although both could be done by traversing the
  ** write-list, in practice this is impractically slow.
  */
  u_int8_t *data;                           /* Buffer containing file contents */
  int n_data;                           /* Size of buffer allocated at data */
  int64_t i_size;                           /* Size of file in bytes */
};

struct crash_global_t {
  write_buffer_t *write_list;     /* Head of write-list */
  write_buffer_t *write_list_end;  /* End of write-list */

  int i_sector_size;             /* Value of simulated sector size */
  int i_device_characteristics;  /* Value of simulated device characteristics */

  int i_crash;                  /* Crash on the i_crash'th call to x_sync() */
  char crash_file[500];        /* Crash during an x_sync() on this file */
};

static crash_global_t g = {0, 0, DBSQL_DEFAULT_SECTOR_SIZE, 0, 0};

/*
** Set this global variable to 1 to enable crash testing.
*/
static int __crash_test_enable = 0;

static void *crash_malloc(int n_byte){
  return (void *)Tcl_AttemptAlloc((size_t)n_byte);
}
static void crash_free(void *p){
  Tcl_Free(p);
}
static void *crash_realloc(void *p, int n){
  return (void *)Tcl_AttemptRealloc(p, (size_t)n);
}

/*
** Wrapper around the __dbsql_os_write() function that avoids writing to the
** 512 byte block beginning at offset PENDING_BYTE.
*/
static int __write_db_file(crash_file_t *p, u_int8_t *z, int64_t i_amt, int64_t i_off){
  int rc = DBSQL_OK;
  int i_skip = 0;
  if( (i_amt-i_skip)>0 ){
    rc = __dbsql_os_write(p->real_file, &z[i_skip], (int)(i_amt-i_skip), i_off+i_skip);
  }
  return rc;
}

/*
** Flush the write-list as if x_sync() had been called on file handle
** file. If is_crash is true, simulate a crash.
*/
static int __write_list_sync(crash_file_t *file, int is_crash){
  int rc = DBSQL_OK;
  int i_dc = g.i_device_characteristics;

  write_buffer_t *p_write;
  write_buffer_t **ptr;

  /* If this is not a crash simulation, set final to point to the
  ** last element of the write-list that is associated with file handle
  ** file.
  **
  ** If this is a crash simulation, set final to an arbitrarily selected
  ** element of the write-list.
  */
  write_buffer_t *final = 0;
  if( !is_crash ){
    for(p_write=g.write_list; p_write; p_write=p_write->next){
      if( p_write->file==file ){
        final = p_write;
      }
    }
  }else if( i_dc&(DBSQL_IOCAP_SEQUENTIAL|DBSQL_IOCAP_SAFE_APPEND) ){
    int n_write = 0;
    int i_final;
    for(p_write=g.write_list; p_write; p_write=p_write->next) n_write++;
    dbsql_randomness(sizeof(int), &i_final);
    i_final = ((i_final<0)?-1*i_final:i_final)%n_write;
    for(p_write=g.write_list; i_final>0; p_write=p_write->next) i_final--;
    final = p_write;
  }

#ifdef TRACE_CRASHTEST
  if( file ){
    printf("Sync %s (is %s crash)\n", file->name, (is_crash?"a":"not a"));
  }
#endif

  ptr = &g.write_list;
  for(p_write=*ptr; rc==DBSQL_OK && p_write; p_write=*ptr){
    dbsql_file_t *real_file = p_write->file->real_file;

    /* (action==1)      -> write block out normally,
    ** (action==2)      -> do nothing,
    ** (action==3)      -> trash sectors.
    */
    int action = 0;
    if( !is_crash ){
      action = 2;
      if( (p_write->file==file || i_dc&DBSQL_IOCAP_SEQUENTIAL) ){
        action = 1;
      }
    }else{
      char random;
      dbsql_randomness(1, &random);

      /* Do not select option 3 (sector trashing) if the IOCAP_ATOMIC flag
      ** is set or this is an __os_truncate(), not an Oswrite().
      */
      if( (i_dc&DBSQL_IOCAP_ATOMIC) || (p_write->buf==0) ){
        random &= 0x01;
      }

      /* If IOCAP_SEQUENTIAL is set and this is not the final entry
      ** in the truncated write-list, always select option 1 (write
      ** out correctly).
      */
      if( (i_dc&DBSQL_IOCAP_SEQUENTIAL && p_write!=final) ){
        random = 0;
      }

      /* If IOCAP_SAFE_APPEND is set and this __os_write() operation is
      ** an append (first byte of the written region is 1 byte past the
      ** current EOF), always select option 1 (write out correctly).
      */
      if( i_dc&DBSQL_IOCAP_SAFE_APPEND && p_write->buf ){
        int64_t i_size;
        __dbsql_os_file_size(real_file, &i_size);
        if( i_size==p_write->i_offset ){
          random = 0;
        }
      }

      if( (random&0x06)==0x06 ){
        action = 3;
      }else{
        action = ((random&0x01)?2:1);
      }
    }

    switch( action ){
      case 1: {               /* Write out correctly */
        if( p_write->buf ){
          rc = __write_db_file(
              p_write->file, p_write->buf, p_write->n_buf, p_write->i_offset
          );
        }else{
          rc = __dbsql_os_truncate(real_file, p_write->i_offset);
        }
        *ptr = p_write->next;
#ifdef TRACE_CRASHTEST
        if( is_crash ){
          printf("Writing %d bytes @ %d (%s)\n",
            p_write->n_buf, (int)p_write->i_offset, p_write->file->name
          );
        }
#endif
        crash_free(p_write);
        break;
      }
      case 2: {               /* Do nothing */
        ptr = &p_write->next;
#ifdef TRACE_CRASHTEST
        if( is_crash ){
          printf("Omiting %d bytes @ %d (%s)\n",
            p_write->n_buf, (int)p_write->i_offset, p_write->file->name
          );
        }
#endif
        break;
      }
      case 3: {               /* Trash sectors */
        u_int8_t *garbage;
        int i_first = (int)(p_write->i_offset/g.i_sector_size);
        int i_last = (int)((p_write->i_offset+p_write->n_buf-1)/g.i_sector_size);

        assert(p_write->buf);

#ifdef TRACE_CRASHTEST
        printf("Trashing %d sectors (%d bytes) @ %lld (sector %d) (%s)\n",
            1+i_last-i_first, (1+i_last-i_first)*g.i_sector_size,
            p_write->i_offset, i_first, p_write->file->name
        );
#endif

        garbage = crash_malloc(g.i_sector_size);
        if( garbage ){
          int64_t i;
          for(i=i_first; rc==DBSQL_OK && i<=i_last; i++){
            dbsql_randomness(g.i_sector_size, garbage);
            rc = __write_db_file(
              p_write->file, garbage, g.i_sector_size, i*g.i_sector_size
            );
          }
          crash_free(garbage);
        }else{
          rc = DBSQL_NOMEM;
        }

        ptr = &p_write->next;
        break;
      }

      default:
        assert(!"Cannot happen");
    }

    if( p_write==final ) break;
  }

  if( rc==DBSQL_OK && is_crash ){
    exit(-1);
  }

  for(p_write=g.write_list; p_write && p_write->next; p_write=p_write->next);
  g.write_list_end = p_write;

  return rc;
}

/*
** Add an entry to the end of the write-list.
*/
static int __write_list_append(
  dbsql_file_t *file,
  int64_t i_offset,
  const u_int8_t *buf,
  int n_buf
){
  write_buffer_t *p_new;

  assert((buf && n_buf) || (!n_buf && !buf));

  p_new = (write_buffer_t *)crash_malloc(sizeof(write_buffer_t) + n_buf);
  if( p_new==0 ){
    fprintf(stderr, "out of memory in the crash simulator\n");
  }
  memset(p_new, 0, sizeof(write_buffer_t)+n_buf);
  p_new->i_offset = i_offset;
  p_new->n_buf = n_buf;
  p_new->file = (crash_file_t *)file;
  if( buf ){
    p_new->buf = (u_int8_t *)&p_new[1];
    memcpy(p_new->buf, buf, n_buf);
  }

  if( g.write_list ){
    assert(g.write_list_end);
    g.write_list_end->next = p_new;
  }else{
    g.write_list = p_new;
  }
  g.write_list_end = p_new;
 
  return DBSQL_OK;
}

/*
** Close a crash-file.
*/
static int __cf_close(dbsql_file_t *file){
  crash_file_t *crash = (crash_file_t *)file;
  __write_list_sync(crash, 0);
  __dbsql_os_close(crash->real_file);
  return DBSQL_OK;
}

/*
** Read data from a crash-file.
*/
static int __cf_read(
  dbsql_file_t *file,
  void *buf,
  int i_amt,
  int64_t i_ofst
){
  crash_file_t *crash = (crash_file_t *)file;
  int n_copy = (int)MIN((int64_t)i_amt, (crash->i_size - i_ofst));

  if( n_copy>0 ){
    memcpy(buf, &crash->data[i_ofst], n_copy);
  }

  /* Check the file-size to see if this is a short-read */
  if( n_copy<i_amt ){
    return DBSQL_IOERR_SHORT_READ;
  }

  return DBSQL_OK;
}

/*
** Write data to a crash-file.
*/
static int __cf_write(
  dbsql_file_t *file,
  const void *buf,
  int i_amt,
  int64_t i_ofst
){
  crash_file_t *crash = (crash_file_t *)file;
  if( i_amt+i_ofst>crash->i_size ){
    crash->i_size = (int)(i_amt+i_ofst);
  }
  while( crash->i_size>crash->n_data ){
    u_int8_t *z_new;
    int n_new = (crash->n_data*2) + 4096;
    z_new = crash_realloc(crash->data, n_new);
    if( !z_new ){
      return DBSQL_NOMEM;
    }
    memset(&z_new[crash->n_data], 0, n_new-crash->n_data);
    crash->n_data = n_new;
    crash->data = z_new;
  }
  memcpy(&crash->data[i_ofst], buf, i_amt);
  return __write_list_append(file, i_ofst, buf, i_amt);
}

/*
** Truncate a crash-file.
*/
static int __cf_truncate(dbsql_file_t *file, int64_t size){
  crash_file_t *crash = (crash_file_t *)file;
  assert(size>=0);
  if( crash->i_size>size ){
    crash->i_size = (int)size;
  }
  return __write_list_append(file, size, 0, 0);
}

/*
** Sync a crash-file.
*/
static int __cf_sync(dbsql_file_t *file, int flags){
  crash_file_t *crash = (crash_file_t *)file;
  int is_crash = 0;

  const char *name = crash->name;
  const char *crash_file = g.crash_file;
  int n_name = (int)strlen(name);
  int n_crash_file = (int)strlen(crash_file);

  if( n_crash_file>0 && crash_file[n_crash_file-1]=='*' ){
    n_crash_file--;
    if( n_name>n_crash_file ) n_name = n_crash_file;
  }

#ifdef TRACE_CRASHTEST
  printf("cfSync(): nName = %d, nCrashFile = %d, zName = %s, zCrashFile = %s\n",
         n_name, n_crash_file, name, crash_file);
#endif

  if( n_name==n_crash_file && 0==memcmp(name, crash_file, n_name) ){
#ifdef TRACE_CRASHTEST
    printf("cfSync(): name matched, g.iCrash = %d\n", g.i_crash);
#endif
    if( (--g.i_crash)==0 ) is_crash = 1;
  }

  return __write_list_sync(crash, is_crash);
}

/*
** Return the current file-size of the crash-file.
*/
static int __cf_file_size(dbsql_file_t *file, int64_t *size){
  crash_file_t *crash = (crash_file_t *)file;
  *size = (int64_t)crash->i_size;
  return DBSQL_OK;
}

/*
** Calls related to file-locks are passed on to the real file handle.
*/
static int __cf_lock(dbsql_file_t *file, int lock){
  return __dbsql_os_lock(((crash_file_t *)file)->real_file, lock);
}
static int __cf_unlock(dbsql_file_t *file, int lock){
  return __dbsql_os_unlock(((crash_file_t *)file)->real_file, lock);
}
static int __cf_check_reserved_lock(dbsql_file_t *file, int *res_out){
  return __dbsql_os_check_reserved_lock(((crash_file_t *)file)->real_file, res_out);
}
static int __cf_file_control(dbsql_file_t *file, int op, void *arg){
  if( op==DBSQL_FCNTL_SIZE_HINT ){
    crash_file_t *crash = (crash_file_t *)file;
    int64_t n_byte = *(int64_t *)arg;
    if( n_byte>crash->i_size ){
      if( DBSQL_OK==__write_list_append(file, n_byte, 0, 0) ){
        crash->i_size = (int)n_byte;
      }
    }
    return DBSQL_OK;
  }
  return __dbsql_os_file_control(((crash_file_t *)file)->real_file, op, arg);
}

/*
** The sector_size() and device_characteristics() functions return
** the global values configured by the [dbsql_crashparams] tcl
*  interface.
*/
static int __cf_sector_size(dbsql_file_t *file){
  return g.i_sector_size;
}
static int __cf_device_characteristics(dbsql_file_t *file){
  return g.i_device_characteristics;
}

/*
** Pass-throughs for WAL support.
*/
static int __cf_shm_lock(dbsql_file_t *file, int ofst, int n, int flags){
  dbsql_file_t *real = ((crash_file_t*)file)->real_file;
  return real->methods->shm_lock(real, ofst, n, flags);
}
static void __cf_shm_barrier(dbsql_file_t *file){
  dbsql_file_t *real = ((crash_file_t*)file)->real_file;
  real->methods->shm_barrier(real);
}
static int __cf_shm_unmap(dbsql_file_t *file, int del_flag){
  dbsql_file_t *real = ((crash_file_t*)file)->real_file;
  return real->methods->shm_unmap(real, del_flag);
}
static int __cf_shm_map(
  dbsql_file_t *file,            /* Handle open on database file */
  int i_region,                    /* Region to retrieve */
  int sz,                         /* Size of regions */
  int w,                          /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  dbsql_file_t *real = ((crash_file_t*)file)->real_file;
  return real->methods->shm_map(real, i_region, sz, w, pp);
}

static const dbsql_io_methods_t __crash_file_vtab = {
  2,                            /* i_version */
  __cf_close,                      /* x_close */
  __cf_read,                       /* x_read */
  __cf_write,                      /* x_write */
  __cf_truncate,                   /* truncate */
  __cf_sync,                       /* x_sync */
  __cf_file_size,                   /* file_size */
  __cf_lock,                       /* lock */
  __cf_unlock,                     /* unlock */
  __cf_check_reserved_lock,          /* check_reserved_lock */
  __cf_file_control,                /* file_control */
  __cf_sector_size,                 /* sector_size */
  __cf_device_characteristics,      /* device_characteristics */
  __cf_shm_map,                     /* shm_map */
  __cf_shm_lock,                    /* shm_lock */
  __cf_shm_barrier,                 /* shm_barrier */
  __cf_shm_unmap                    /* shm_unmap */
};

/*
** Application data for the crash VFS
*/
struct crash_app_data {
  dbsql_vfs_t *orig;                   /* Wrapped vfs structure */
};

/*
** Open a crash-file file handle.
**
** The caller will have allocated vfs->sz_os_file bytes of space
** at file. This file uses this space for the crash_file_t structure
** and allocates space for the "real" file structure using
** dbsql_malloc(). The assumption here is (vfs->sz_os_file) is
** equal or greater than sizeof(crash_file_t).
*/
static int __cf_open(
  dbsql_vfs_t *cf_vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  int rc;
  crash_file_t *wrapper = (crash_file_t *)file;
  dbsql_file_t *real = (dbsql_file_t*)&wrapper[1];

  memset(wrapper, 0, sizeof(crash_file_t));
  rc = __dbsql_os_open(vfs, name, real, flags, out_flags);

  if( rc==DBSQL_OK ){
    int64_t i_size;
    wrapper->method = &__crash_file_vtab;
    wrapper->name = (char *)name;
    wrapper->real_file = real;
    rc = __dbsql_os_file_size(real, &i_size);
    wrapper->i_size = (int)i_size;
    wrapper->flags = flags;
  }
  if( rc==DBSQL_OK ){
    wrapper->n_data = (int)(4096 + wrapper->i_size);
    wrapper->data = crash_malloc(wrapper->n_data);
    if( wrapper->data ){
      /* os_unix.c contains an assert() that fails if the caller attempts
      ** to read data from the 512-byte locking region of a file opened
      ** with the DBSQL_OPEN_MAIN_DB flag. This region of a database file
      ** never contains valid data anyhow. So avoid doing such a read here.
      **
      ** UPDATE: It also contains an assert() verifying that each call
      ** to the x_read() method reads less than 128KB of data.
      */
      int64_t i_off;

      memset(wrapper->data, 0, wrapper->n_data);
      for(i_off=0; i_off<wrapper->i_size; i_off += 512){
        int n_read = (int)(wrapper->i_size - i_off);
        if( n_read>512 ) n_read = 512;
        rc = __dbsql_os_read(real, &wrapper->data[i_off], n_read, i_off);
      }
    }else{
      rc = DBSQL_NOMEM;
    }
  }
  if( rc!=DBSQL_OK && wrapper->method ){
    __dbsql_os_close(file);
  }
  return rc;
}

static int __cf_delete(dbsql_vfs_t *cf_vfs, const char *path, int dir_sync){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->x_delete(vfs, path, dir_sync);
}
static int __cf_access(
  dbsql_vfs_t *cf_vfs,
  const char *path,
  int flags,
  int *res_out
){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->x_access(vfs, path, flags, res_out);
}
static int __cf_full_pathname(
  dbsql_vfs_t *cf_vfs,
  const char *path,
  int n_path_out,
  char *path_out
){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->full_pathname(vfs, path, n_path_out, path_out);
}
static void *__cf_dl_open(dbsql_vfs_t *cf_vfs, const char *path){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->dl_open(vfs, path);
}
static void __cf_dl_error(dbsql_vfs_t *cf_vfs, int n_byte, char *errmsg){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  vfs->dl_error(vfs, n_byte, errmsg);
}
static void (*cf_dl_sym(dbsql_vfs_t *cf_vfs, void *h, const char *sym))(void){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->dl_sym(vfs, h, sym);
}
static void __cf_dl_close(dbsql_vfs_t *cf_vfs, void *handle){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  vfs->dl_close(vfs, handle);
}
static int __cf_randomness(dbsql_vfs_t *cf_vfs, int n_byte, char *buf_out){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->randomness(vfs, n_byte, buf_out);
}
static int __cf_sleep(dbsql_vfs_t *cf_vfs, int n_micro){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->x_sleep(vfs, n_micro);
}
static int __cf_current_time(dbsql_vfs_t *cf_vfs, double *time_out){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->current_time(vfs, time_out);
}
static int __cf_get_last_error(dbsql_vfs_t *cf_vfs, int n, char *z){
  dbsql_vfs_t *vfs = (dbsql_vfs_t *)cf_vfs->app_data;
  return vfs->get_last_error(vfs, n, z);
}

static int __process_dev_sym_args(
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[],
  int *pi_device_char,
  int *pi_sector_size
){
  struct device_flag_t {
    char *name;
    int i_value;
  } flags[] = {
    { "atomic",              DBSQL_IOCAP_ATOMIC                },
    { "atomic512",           DBSQL_IOCAP_ATOMIC512             },
    { "atomic1k",            DBSQL_IOCAP_ATOMIC1K              },
    { "atomic2k",            DBSQL_IOCAP_ATOMIC2K              },
    { "atomic4k",            DBSQL_IOCAP_ATOMIC4K              },
    { "atomic8k",            DBSQL_IOCAP_ATOMIC8K              },
    { "atomic16k",           DBSQL_IOCAP_ATOMIC16K             },
    { "atomic32k",           DBSQL_IOCAP_ATOMIC32K             },
    { "atomic64k",           DBSQL_IOCAP_ATOMIC64K             },
    { "sequential",          DBSQL_IOCAP_SEQUENTIAL            },
    { "safe_append",         DBSQL_IOCAP_SAFE_APPEND           },
    { "powersafe_overwrite", DBSQL_IOCAP_POWERSAFE_OVERWRITE   },
    { "batch-atomic",        DBSQL_IOCAP_BATCH_ATOMIC          },
    { 0, 0 }
  };

  int i;
  int i_dc = 0;
  int i_sector_size = 0;
  int set_sectorsize = 0;
  int set_device_char = 0;

  for(i=0; i<objc; i+=2){
    TCL_SIZE n_opt;
    char *opt = Tcl_GetStringFromObj(objv[i], &n_opt);

    if( (n_opt>11 || n_opt<2 || strncmp("-sectorsize", opt, n_opt))
     && (n_opt>16 || n_opt<2 || strncmp("-characteristics", opt, n_opt))
    ){
      Tcl_AppendResult(interp,
        "Bad option: \"", opt,
        "\" - must be \"-characteristics\" or \"-sectorsize\"", NULL
      );
      return TCL_ERROR;
    }
    if( i==objc-1 ){
      Tcl_AppendResult(interp, "Option requires an argument: \"", opt, "\"", NULL);
      return TCL_ERROR;
    }

    if( opt[1]=='s' ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &i_sector_size) ){
        return TCL_ERROR;
      }
      set_sectorsize = 1;
    }else{
      int j;
      Tcl_Obj **objs;
      TCL_SIZE n_obj;
      if( Tcl_ListObjGetElements(interp, objv[i+1], &n_obj, &objs) ){
        return TCL_ERROR;
      }
      for(j=0; j<(int)n_obj; j++){
        int rc;
        int i_choice;
        Tcl_Obj *flag = Tcl_DuplicateObj(objs[j]);
        Tcl_IncrRefCount(flag);
        Tcl_UtfToLower(Tcl_GetString(flag));

        rc = Tcl_GetIndexFromObjStruct(
            interp, flag, flags, sizeof(flags[0]), "no such flag", 0, &i_choice
        );
        Tcl_DecrRefCount(flag);
        if( rc ){
          return TCL_ERROR;
        }

        i_dc |= flags[i_choice].i_value;
      }
      set_device_char = 1;
    }
  }

  if( set_device_char ){
    *pi_device_char = i_dc;
  }
  if( set_sectorsize ){
    *pi_sector_size = i_sector_size;
  }

  return TCL_OK;
}

/*
** tclcmd:   dbsql_crash_now
**
** Simulate a crash immediately. This function does not return
** (__write_list_sync() calls exit(-1)).
*/
static int DBSQL_TCLAPI __crash_now_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
  __write_list_sync(0, 1);
  assert( 0 );
  return TCL_OK;
}

/*
** tclcmd:   dbsql_crash_enable ENABLE ?DEFAULT?
**
** Parameter ENABLE must be a boolean value. If true, then the "crash"
** vfs is added to the system. If false, it is removed.
*/
static int DBSQL_TCLAPI __crash_enable_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int is_enable;
  int is_default = 0;
  static dbsql_vfs_t crash_vfs = {
    2,                  /* i_version */
    0,                  /* sz_os_file */
    0,                  /* mx_pathname */
    0,                  /* next */
    "crash",            /* name */
    0,                  /* app_data */
 
    __cf_open,               /* x_open */
    __cf_delete,             /* x_delete */
    __cf_access,             /* x_access */
    __cf_full_pathname,       /* full_pathname */
    __cf_dl_open,             /* dl_open */
    __cf_dl_error,            /* dl_error */
    cf_dl_sym,              /* dl_sym */
    __cf_dl_close,            /* dl_close */
    __cf_randomness,         /* randomness */
    __cf_sleep,              /* x_sleep */
    __cf_current_time,        /* current_time */
    __cf_get_last_error,       /* get_last_error */
    0,                    /* current_time_int64 */
  };

  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "ENABLE ?DEFAULT?");
    return TCL_ERROR;
  }

  if( Tcl_GetBooleanFromObj(interp, objv[1], &is_enable) ){
    return TCL_ERROR;
  }
  if( objc==3 && Tcl_GetBooleanFromObj(interp, objv[2], &is_default) ){
    return TCL_ERROR;
  }

  if( (is_enable && crash_vfs.app_data) || (!is_enable && !crash_vfs.app_data) ){
    return TCL_OK;
  }

  if( crash_vfs.app_data==0 ){
    dbsql_vfs_t *original_vfs = dbsql_vfs_find(0);
    crash_vfs.mx_pathname = original_vfs->mx_pathname;
    crash_vfs.app_data = (void *)original_vfs;
    crash_vfs.sz_os_file = sizeof(crash_file_t) + original_vfs->sz_os_file;
    dbsql_vfs_register(&crash_vfs, is_default);
  }else{
    crash_vfs.app_data = 0;
    dbsql_vfs_unregister(&crash_vfs);
  }

  return TCL_OK;
}

/*
** tclcmd:   dbsql_crashparams ?OPTIONS? DELAY CRASHFILE
**
** This procedure implements a TCL command that enables crash testing
** in testfixture.  Once enabled, crash testing cannot be disabled.
**
** Available options are "-characteristics" and "-sectorsize". Both require
** an argument. For -sectorsize, this is the simulated sector size in
** bytes. For -characteristics, the argument must be a list of io-capability
** flags to simulate. Valid flags are "atomic", "atomic512", "atomic1K",
** "atomic2K", "atomic4K", "atomic8K", "atomic16K", "atomic32K",
** "atomic64K", "sequential" and "safe_append".
**
** Example:
**
**   dbsql_crashparams -sect 1024 -char {atomic sequential} ./test.db 1
**
*/
static int DBSQL_TCLAPI __crash_params_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int i_delay;
  const char *crash_file;
  TCL_SIZE n_crash_file;
  int i_dc, i_sector_size;

  i_dc = -1;
  i_sector_size = -1;

  if( objc<3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?OPTIONS? DELAY CRASHFILE");
    goto error;
  }

  crash_file = Tcl_GetStringFromObj(objv[objc-1], &n_crash_file);
  if( n_crash_file>=sizeof(g.crash_file) ){
    Tcl_AppendResult(interp, "Filename is too long: \"", crash_file, "\"", NULL);
    goto error;
  }
  if( Tcl_GetIntFromObj(interp, objv[objc-2], &i_delay) ){
    goto error;
  }

  if( __process_dev_sym_args(interp, objc-3, &objv[1], &i_dc, &i_sector_size) ){
    return TCL_ERROR;
  }

  if( i_dc>=0 ){
    g.i_device_characteristics = i_dc;
  }
  if( i_sector_size>=0 ){
    g.i_sector_size = i_sector_size;
  }

  g.i_crash = i_delay;
  memcpy(g.crash_file, crash_file, n_crash_file+1);
  __crash_test_enable = 1;
  return TCL_OK;

error:
  return TCL_ERROR;
}

static int DBSQL_TCLAPI __dev_sym_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void devsym_register(int i_device_char, int i_sector_size);

  int i_dc = -1;
  int i_sector_size = -1;

  if( __process_dev_sym_args(interp, objc-1, &objv[1], &i_dc, &i_sector_size) ){
    return TCL_ERROR;
  }
  devsym_register(i_dc, i_sector_size);

  return TCL_OK;
}

/*
** tclcmd: dbsql_crash_on_write N
*/
static int DBSQL_TCLAPI __write_crash_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void devsym_crash_on_write(int);
  int n_write = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NWRITE");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &n_write) ){
    return TCL_ERROR;
  }

  devsym_crash_on_write(n_write);
  return TCL_OK;
}

/*
** tclcmd: unregister_devsim
*/
static int DBSQL_TCLAPI __ds_unregister_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void devsym_unregister(void);

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  devsym_unregister();
  return TCL_OK;
}

/*
** tclcmd: register_jt_vfs ?-default? PARENT-VFS
*/
static int DBSQL_TCLAPI __jt_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int jt_register(char *, int);
  char *parent = 0;

  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-default? PARENT-VFS");
    return TCL_ERROR;
  }
  parent = Tcl_GetString(objv[1]);
  if( objc==3 ){
    if( strcmp(parent, "-default") ){
      Tcl_AppendResult(interp,
          "bad option \"", parent, "\": must be -default", NULL
      );
      return TCL_ERROR;
    }
    parent = Tcl_GetString(objv[2]);
  }

  if( !(*parent) ){
    parent = 0;
  }
  if( jt_register(parent, objc==3) ){
    Tcl_AppendResult(interp, "Error in jt_register", NULL);
    return TCL_ERROR;
  }

  return TCL_OK;
}

/*
** tclcmd: unregister_jt_vfs
*/
static int DBSQL_TCLAPI __jt_unregister_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void jt_unregister(void);

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  jt_unregister();
  return TCL_OK;
}

#endif /* DBSQL_OMIT_DISKIO */

/*
** This procedure registers the TCL procedures defined in this file.
*/
int Dbsqltest6_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_DISKIO
  Tcl_CreateObjCommand(interp, "dbsql_crash_enable", __crash_enable_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_crashparams", __crash_params_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_crash_now", __crash_now_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_simulate_device", __dev_sym_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_crash_on_write", __write_crash_obj_cmd,0,0);
  Tcl_CreateObjCommand(interp, "unregister_devsim", __ds_unregister_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "register_jt_vfs", __jt_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "unregister_jt_vfs", __jt_unregister_obj_cmd, 0, 0);
#endif
  return TCL_OK;
}

#endif /* DBSQL_TEST */
