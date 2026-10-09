/*
** 2007 September 14
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
** OVERVIEW:
**
**   This file contains some example code demonstrating how the dbsql 
**   vfs feature can be used to have dbsql operate directly on an 
**   embedded media, without using an intermediate file system.
**
**   Because this is only a demo designed to run on a workstation, the
**   underlying media is simulated using a regular file-system file. The
**   size of the file is fixed when it is first created (default size 10 MB).
**   From dbsql's point of view, this space is used to store a single
**   database file and the journal file. 
**
**   Any statement journal created is stored in volatile memory obtained 
**   from dbsql_malloc(). Any attempt to create a temporary database file 
**   will fail (DBSQL_IOERR). To prevent dbsql from attempting this,
**   it should be configured to store all temporary database files in 
**   main memory (see pragma "temp_store" or the DBSQL_TEMP_STORE compile 
**   time option).
**
** ASSUMPTIONS:
**
**   After it has been created, the blob file is accessed using the
**   following three functions only:
**
**       media_read();            - Read a 512 byte block from the file.
**       media_write();           - Write a 512 byte block to the file.
**       media_sync();            - Tell the media hardware to sync.
**
**   It is assumed that these can be easily implemented by any "real"
**   media vfs driver adapting this code.
**
** FILE FORMAT:
**
**   The basic principle is that the "database file" is stored at the
**   beginning of the 10 MB blob and grows in a forward direction. The 
**   "journal file" is stored at the end of the 10MB blob and grows
**   in the reverse direction. If, during a transaction, insufficient
**   space is available to expand either the journal or database file,
**   an DBSQL_FULL error is returned. The database file is never allowed
**   to consume more than 90% of the blob space. If dbsql tries to
**   create a file larger than this, DBSQL_FULL is returned.
**
**   No allowance is made for "wear-leveling", as is required by.
**   embedded devices in the absence of equivalent hardware features.
**
**   The first 512 block byte of the file is reserved for storing the
**   size of the "database file". It is updated as part of the sync()
**   operation. On startup, it can only be trusted if no journal file
**   exists. If a journal-file does exist, then it stores the real size
**   of the database region. The second and subsequent blocks store the 
**   actual database content.
**
**   The size of the "journal file" is not stored persistently in the 
**   file. When the system is running, the size of the journal file is
**   stored in volatile memory. When recovering from a crash, this vfs
**   reports a very large size for the journal file. The normal journal
**   header and checksum mechanisms serve to prevent dbsql from 
**   processing any data that lies past the logical end of the journal.
**
**   When dbsql calls __os_delete() to delete the journal file, the final
**   512 bytes of the blob (the area containing the first journal header)
**   are zeroed.
**
** LOCKING:
**
**   File locking is a no-op. Only one connection may be open at any one
**   time using this demo vfs.
*/

#include "dbsql.h"
#include <assert.h>
#include <string.h>

/*
** Maximum pathname length supported by the fs backend.
*/
#define BLOCKSIZE 512
#define BLOBSIZE 10485760

/*
** Name used to identify this VFS.
*/
#define FS_VFS_NAME "fs"

typedef struct fs_real_file fs_real_file;
struct fs_real_file {
  dbsql_file_t *file;
  const char *name;
  int n_database;              /* Current size of database region */
  int n_journal;               /* Current size of journal region */
  int n_blob;                  /* Total size of allocated blob */
  int n_ref;                   /* Number of pointers to this structure */
  fs_real_file *next;
  fs_real_file **pp_this;
};

typedef struct fs_file fs_file;
struct fs_file {
  dbsql_file_t base;
  int type;
  fs_real_file *real;
};

typedef struct tmp_file tmp_file;
struct tmp_file {
  dbsql_file_t base;
  int n_size;
  int n_alloc;
  char *alloc;
};

/* Values for fs_file.type. */
#define DATABASE_FILE   1
#define JOURNAL_FILE    2

/*
** Method declarations for fs_file.
*/
static int __fs_close(dbsql_file_t*);
static int __fs_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __fs_write(dbsql_file_t*, const void*, int i_amt, int64_t i_ofst);
static int __fs_truncate(dbsql_file_t*, int64_t size);
static int __fs_sync(dbsql_file_t*, int flags);
static int __fs_file_size(dbsql_file_t*, int64_t *size);
static int __fs_lock(dbsql_file_t*, int);
static int __fs_unlock(dbsql_file_t*, int);
static int __fs_check_reserved_lock(dbsql_file_t*, int *res_out);
static int __fs_file_control(dbsql_file_t*, int op, void *arg);
static int __fs_sector_size(dbsql_file_t*);
static int __fs_device_characteristics(dbsql_file_t*);

/*
** Method declarations for tmp_file.
*/
static int __tmp_close(dbsql_file_t*);
static int __tmp_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __tmp_write(dbsql_file_t*, const void*, int i_amt, int64_t i_ofst);
static int __tmp_truncate(dbsql_file_t*, int64_t size);
static int __tmp_sync(dbsql_file_t*, int flags);
static int __tmp_file_size(dbsql_file_t*, int64_t *size);
static int __tmp_lock(dbsql_file_t*, int);
static int __tmp_unlock(dbsql_file_t*, int);
static int __tmp_check_reserved_lock(dbsql_file_t*, int *res_out);
static int __tmp_file_control(dbsql_file_t*, int op, void *arg);
static int __tmp_sector_size(dbsql_file_t*);
static int __tmp_device_characteristics(dbsql_file_t*);

/*
** Method declarations for fs_vfs.
*/
static int __fs_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __fs_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __fs_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __fs_full_pathname(dbsql_vfs_t*, const char *name, int n_out,char *out);
static void *__fs_dl_open(dbsql_vfs_t*, const char *filename);
static void __fs_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*fs_dl_sym(dbsql_vfs_t*,void*, const char *symbol))(void);
static void __fs_dl_close(dbsql_vfs_t*, void*);
static int __fs_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __fs_sleep(dbsql_vfs_t*, int microseconds);
static int __fs_current_time(dbsql_vfs_t*, double*);


typedef struct fs_vfs_t fs_vfs_t;
struct fs_vfs_t {
  dbsql_vfs_t base;
  fs_real_file *file_list;
  dbsql_vfs_t *parent;
};

static fs_vfs_t fs_vfs = {
  {
    1,                                          /* i_version */
    0,                                          /* sz_os_file */
    0,                                          /* mx_pathname */
    0,                                          /* next */
    FS_VFS_NAME,                                /* name */
    0,                                          /* app_data */
    __fs_open,                                     /* x_open */
    __fs_delete,                                   /* x_delete */
    __fs_access,                                   /* x_access */
    __fs_full_pathname,                             /* full_pathname */
    __fs_dl_open,                                   /* dl_open */
    __fs_dl_error,                                  /* dl_error */
    fs_dl_sym,                                    /* dl_sym */
    __fs_dl_close,                                  /* dl_close */
    __fs_randomness,                               /* randomness */
    __fs_sleep,                                    /* x_sleep */
    __fs_current_time,                              /* current_time */
    0                                           /* current_time_int64 */
  }, 
  0,                                            /* file_list */
  0                                             /* parent */
};

static dbsql_io_methods_t fs_io_methods = {
  1,                            /* i_version */
  __fs_close,                      /* x_close */
  __fs_read,                       /* x_read */
  __fs_write,                      /* x_write */
  __fs_truncate,                   /* truncate */
  __fs_sync,                       /* x_sync */
  __fs_file_size,                   /* file_size */
  __fs_lock,                       /* lock */
  __fs_unlock,                     /* unlock */
  __fs_check_reserved_lock,          /* check_reserved_lock */
  __fs_file_control,                /* file_control */
  __fs_sector_size,                 /* sector_size */
  __fs_device_characteristics,      /* device_characteristics */
  0,                            /* shm_map */
  0,                            /* shm_lock */
  0,                            /* shm_barrier */
  0                             /* shm_unmap */
};


static dbsql_io_methods_t tmp_io_methods = {
  1,                            /* i_version */
  __tmp_close,                     /* x_close */
  __tmp_read,                      /* x_read */
  __tmp_write,                     /* x_write */
  __tmp_truncate,                  /* truncate */
  __tmp_sync,                      /* x_sync */
  __tmp_file_size,                  /* file_size */
  __tmp_lock,                      /* lock */
  __tmp_unlock,                    /* unlock */
  __tmp_check_reserved_lock,         /* check_reserved_lock */
  __tmp_file_control,               /* file_control */
  __tmp_sector_size,                /* sector_size */
  __tmp_device_characteristics,     /* device_characteristics */
  0,                            /* shm_map */
  0,                            /* shm_lock */
  0,                            /* shm_barrier */
  0                             /* shm_unmap */
};

/* Useful macros used in several places */
#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))


/*
** Close a tmp-file.
*/
static int __tmp_close(dbsql_file_t *file){
  tmp_file *tmp = (tmp_file *)file;
  dbsql_free(tmp->alloc);
  return DBSQL_OK;
}

/*
** Read data from a tmp-file.
*/
static int __tmp_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  tmp_file *tmp = (tmp_file *)file;
  if( (i_amt+i_ofst)>tmp->n_size ){
    return DBSQL_IOERR_SHORT_READ;
  }
  memcpy(buf, &tmp->alloc[i_ofst], i_amt);
  return DBSQL_OK;
}

/*
** Write data to a tmp-file.
*/
static int __tmp_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  tmp_file *tmp = (tmp_file *)file;
  if( (i_amt+i_ofst)>tmp->n_alloc ){
    int n_new = (int)(2*(i_amt+i_ofst+tmp->n_alloc));
    char *z_new = dbsql_realloc(tmp->alloc, n_new);
    if( !z_new ){
      return DBSQL_NOMEM;
    }
    tmp->alloc = z_new;
    tmp->n_alloc = n_new;
  }
  memcpy(&tmp->alloc[i_ofst], buf, i_amt);
  tmp->n_size = (int)MAX(tmp->n_size, i_ofst+i_amt);
  return DBSQL_OK;
}

/*
** Truncate a tmp-file.
*/
static int __tmp_truncate(dbsql_file_t *file, int64_t size){
  tmp_file *tmp = (tmp_file *)file;
  tmp->n_size = (int)MIN(tmp->n_size, size);
  return DBSQL_OK;
}

/*
** Sync a tmp-file.
*/
static int __tmp_sync(dbsql_file_t *file, int flags){
  return DBSQL_OK;
}

/*
** Return the current file-size of a tmp-file.
*/
static int __tmp_file_size(dbsql_file_t *file, int64_t *size){
  tmp_file *tmp = (tmp_file *)file;
  *size = tmp->n_size;
  return DBSQL_OK;
}

/*
** Lock a tmp-file.
*/
static int __tmp_lock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}

/*
** Unlock a tmp-file.
*/
static int __tmp_unlock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on a tmp-file.
*/
static int __tmp_check_reserved_lock(dbsql_file_t *file, int *res_out){
  *res_out = 0;
  return DBSQL_OK;
}

/*
** File control method. For custom operations on a tmp-file.
*/
static int __tmp_file_control(dbsql_file_t *file, int op, void *arg){
  return DBSQL_OK;
}

/*
** Return the sector-size in bytes for a tmp-file.
*/
static int __tmp_sector_size(dbsql_file_t *file){
  return 0;
}

/*
** Return the device characteristic flags supported by a tmp-file.
*/
static int __tmp_device_characteristics(dbsql_file_t *file){
  return 0;
}

/*
** Close an fs-file.
*/
static int __fs_close(dbsql_file_t *file){
  int rc = DBSQL_OK;
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;

  /* Decrement the real_file ref-count. */
  real->n_ref--;
  assert(real->n_ref>=0);

  /* When the ref-count reaches 0, destroy the structure */
  if( real->n_ref==0 ){
    *real->pp_this = real->next;
    if( real->next ){
      real->next->pp_this = real->pp_this;
    }
    rc = real->file->methods->x_close(real->file);
    dbsql_free(real);
  }

  return rc;
}

/*
** Read data from an fs-file.
*/
static int __fs_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc = DBSQL_OK;
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;
  dbsql_file_t *f = real->file;

  if( (p->type==DATABASE_FILE && (i_amt+i_ofst)>real->n_database)
   || (p->type==JOURNAL_FILE && (i_amt+i_ofst)>real->n_journal)
  ){
    rc = DBSQL_IOERR_SHORT_READ;
  }else if( p->type==DATABASE_FILE ){
    rc = f->methods->x_read(f, buf, i_amt, i_ofst+BLOCKSIZE);
  }else{
    /* Journal file. */
    int i_rem = i_amt;
    int i_buf = 0;
    int ii = (int)i_ofst;
    while( i_rem>0 && rc==DBSQL_OK ){
      int i_real_off = real->n_blob - BLOCKSIZE*((ii/BLOCKSIZE)+1) + ii%BLOCKSIZE;
      int i_real_amt = MIN(i_rem, BLOCKSIZE - (i_real_off%BLOCKSIZE));

      rc = f->methods->x_read(f, &((char *)buf)[i_buf], i_real_amt, i_real_off);
      ii += i_real_amt;
      i_buf += i_real_amt;
      i_rem -= i_real_amt;
    }
  }

  return rc;
}

/*
** Write data to an fs-file.
*/
static int __fs_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc = DBSQL_OK;
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;
  dbsql_file_t *f = real->file;

  if( p->type==DATABASE_FILE ){
    if( (i_amt+i_ofst+BLOCKSIZE)>(real->n_blob-real->n_journal) ){
      rc = DBSQL_FULL;
    }else{
      rc = f->methods->x_write(f, buf, i_amt, i_ofst+BLOCKSIZE);
      if( rc==DBSQL_OK ){
        real->n_database = (int)MAX(real->n_database, i_amt+i_ofst);
      }
    }
  }else{
    /* Journal file. */
    int i_rem = i_amt;
    int i_buf = 0;
    int ii = (int)i_ofst;
    while( i_rem>0 && rc==DBSQL_OK ){
      int i_real_off = real->n_blob - BLOCKSIZE*((ii/BLOCKSIZE)+1) + ii%BLOCKSIZE;
      int i_real_amt = MIN(i_rem, BLOCKSIZE - (i_real_off%BLOCKSIZE));

      if( i_real_off<(real->n_database+BLOCKSIZE) ){
        rc = DBSQL_FULL;
      }else{
        rc = f->methods->x_write(f, &((char *)buf)[i_buf], i_real_amt,i_real_off);
        ii += i_real_amt;
        i_buf += i_real_amt;
        i_rem -= i_real_amt;
      }
    }
    if( rc==DBSQL_OK ){
      real->n_journal = (int)MAX(real->n_journal, i_amt+i_ofst);
    }
  }

  return rc;
}

/*
** Truncate an fs-file.
*/
static int __fs_truncate(dbsql_file_t *file, int64_t size){
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;
  if( p->type==DATABASE_FILE ){
    real->n_database = (int)MIN(real->n_database, size);
  }else{
    real->n_journal = (int)MIN(real->n_journal, size);
  }
  return DBSQL_OK;
}

/*
** Sync an fs-file.
*/
static int __fs_sync(dbsql_file_t *file, int flags){
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;
  dbsql_file_t *real_file = real->file;
  int rc = DBSQL_OK;

  if( p->type==DATABASE_FILE ){
    unsigned char size[4];
    size[0] = (real->n_database&0xFF000000)>>24;
    size[1] = (unsigned char)((real->n_database&0x00FF0000)>>16);
    size[2] = (real->n_database&0x0000FF00)>>8;
    size[3] = (real->n_database&0x000000FF);
    rc = real_file->methods->x_write(real_file, size, 4, 0);
  }
  if( rc==DBSQL_OK ){
    rc = real_file->methods->x_sync(real_file, flags&(~DBSQL_SYNC_DATAONLY));
  }

  return rc;
}

/*
** Return the current file-size of an fs-file.
*/
static int __fs_file_size(dbsql_file_t *file, int64_t *size){
  fs_file *p = (fs_file *)file;
  fs_real_file *real = p->real;
  if( p->type==DATABASE_FILE ){
    *size = real->n_database;
  }else{
    *size = real->n_journal;
  }
  return DBSQL_OK;
}

/*
** Lock an fs-file.
*/
static int __fs_lock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}

/*
** Unlock an fs-file.
*/
static int __fs_unlock(dbsql_file_t *file, int lock){
  return DBSQL_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on an fs-file.
*/
static int __fs_check_reserved_lock(dbsql_file_t *file, int *res_out){
  *res_out = 0;
  return DBSQL_OK;
}

/*
** File control method. For custom operations on an fs-file.
*/
static int __fs_file_control(dbsql_file_t *file, int op, void *arg){
  if( op==DBSQL_FCNTL_PRAGMA ) return DBSQL_NOTFOUND;
  return DBSQL_OK;
}

/*
** Return the sector-size in bytes for an fs-file.
*/
static int __fs_sector_size(dbsql_file_t *file){
  return BLOCKSIZE;
}

/*
** Return the device characteristic flags supported by an fs-file.
*/
static int __fs_device_characteristics(dbsql_file_t *file){
  return 0;
}

/*
** Open an fs file handle.
*/
static int __fs_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  fs_vfs_t *p_fs_vfs = (fs_vfs_t *)vfs;
  fs_file *p = (fs_file *)file;
  fs_real_file *real = 0;
  int type;
  int n_name;
  int rc = DBSQL_OK;

  if( 0==(flags&(DBSQL_OPEN_MAIN_DB|DBSQL_OPEN_MAIN_JOURNAL)) ){
    tmp_file *p2 = (tmp_file *)file;
    memset(p2, 0, sizeof(*p2));
    p2->base.methods = &tmp_io_methods;
    return DBSQL_OK;
  }

  type = ((flags&(DBSQL_OPEN_MAIN_DB))?DATABASE_FILE:JOURNAL_FILE);
  p->base.methods = &fs_io_methods;
  p->type = type;

  assert(strlen("-journal")==8);
  n_name = (int)strlen(name)-((type==JOURNAL_FILE)?8:0);
  real=p_fs_vfs->file_list; 
  for(; real && strncmp(real->name, name, n_name); real=real->next);

  if( !real ){
    int real_flags = (flags&~(DBSQL_OPEN_MAIN_DB))|DBSQL_OPEN_TEMP_DB;
    int64_t size;
    dbsql_file_t *real_file;
    dbsql_vfs_t *parent = p_fs_vfs->parent;
    assert(type==DATABASE_FILE);

    real = (fs_real_file *)dbsql_malloc(sizeof(*real)+parent->sz_os_file);
    if( !real ){
      rc = DBSQL_NOMEM;
      goto open_out;
    }
    memset(real, 0, sizeof(*real)+parent->sz_os_file);
    real->name = name;
    real->file = (dbsql_file_t *)(&real[1]);

    rc = parent->x_open(parent, name, real->file, real_flags, out_flags);
    if( rc!=DBSQL_OK ){
      goto open_out;
    }
    real_file = real->file;

    rc = real_file->methods->file_size(real_file, &size);
    if( rc!=DBSQL_OK ){
      goto open_out;
    }
    if( size==0 ){
      rc = real_file->methods->x_write(real_file, "\0", 1, BLOBSIZE-1);
      real->n_blob = BLOBSIZE;
    }else{
      unsigned char s[4];
      real->n_blob = (int)size;
      rc = real_file->methods->x_read(real_file, s, 4, 0);
      real->n_database = (s[0]<<24)+(s[1]<<16)+(s[2]<<8)+s[3];
      if( rc==DBSQL_OK ){
        rc = real_file->methods->x_read(real_file, s, 4, real->n_blob-4);
        if( s[0] || s[1] || s[2] || s[3] ){
          real->n_journal = real->n_blob;
        }
      }
    }

    if( rc==DBSQL_OK ){
      real->next = p_fs_vfs->file_list;
      if( real->next ){
        real->next->pp_this = &real->next;
      }
      real->pp_this = &p_fs_vfs->file_list;
      p_fs_vfs->file_list = real;
    }
  }

open_out:
  if( real ){
    if( rc==DBSQL_OK ){
      p->real = real;
      real->n_ref++;
    }else{
      if( real->file->methods ){
        real->file->methods->x_close(real->file);
      }
      dbsql_free(real);
    }
  }
  return rc;
}

/*
** Delete the file located at path. If the dir_sync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int __fs_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  int rc = DBSQL_OK;
  fs_vfs_t *p_fs_vfs = (fs_vfs_t *)vfs;
  fs_real_file *real;
  dbsql_file_t *f;
  int n_name = (int)strlen(path) - 8;

  assert(strlen("-journal")==8);
  assert(strcmp("-journal", &path[n_name])==0);

  real = p_fs_vfs->file_list; 
  for(; real && strncmp(real->name, path, n_name); real=real->next);
  if( real ){
    f = real->file;
    rc = f->methods->x_write(f, "\0\0\0\0", 4, real->n_blob-BLOCKSIZE);
    if( rc==DBSQL_OK ){
      real->n_journal = 0;
    }
  }
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int __fs_access(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int flags, 
  int *res_out
){
  fs_vfs_t *p_fs_vfs = (fs_vfs_t *)vfs;
  fs_real_file *real;
  int is_journal = 0;
  int n_name = (int)strlen(path);

  if( flags!=DBSQL_ACCESS_EXISTS ){
    dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
    return parent->x_access(parent, path, flags, res_out);
  }

  assert(strlen("-journal")==8);
  if( n_name>8 && strcmp("-journal", &path[n_name-8])==0 ){
    n_name -= 8;
    is_journal = 1;
  }

  real = p_fs_vfs->file_list; 
  for(; real && strncmp(real->name, path, n_name); real=real->next);

  *res_out = (real && (!is_journal || real->n_journal>0));
  return DBSQL_OK;
}

/*
** Populate buffer out with the full canonical pathname corresponding
** to the pathname in path. out is guaranteed to point to a buffer
** of at least (FS_MAX_PATHNAME+1) bytes.
*/
static int __fs_full_pathname(
  dbsql_vfs_t *vfs,            /* Pointer to vfs object */
  const char *path,            /* Possibly relative input path */
  int n_out,                     /* Size of output buffer in bytes */
  char *out                    /* Output buffer */
){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->full_pathname(parent, path, n_out, out);
}

/*
** Open the dynamic library located at path and return a handle.
*/
static void *__fs_dl_open(dbsql_vfs_t *vfs, const char *path){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->dl_open(parent, path);
}

/*
** Populate the buffer errmsg (size n_byte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void __fs_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  parent->dl_error(parent, n_byte, errmsg);
}

/*
** Return a pointer to the symbol symbol in the dynamic library handle.
*/
static void (*fs_dl_sym(dbsql_vfs_t *vfs, void *h, const char *sym))(void){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->dl_sym(parent, h, sym);
}

/*
** Close the dynamic library handle handle.
*/
static void __fs_dl_close(dbsql_vfs_t *vfs, void *handle){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  parent->dl_close(parent, handle);
}

/*
** Populate the buffer pointed to by buf_out with n_byte bytes of 
** random data.
*/
static int __fs_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->randomness(parent, n_byte, buf_out);
}

/*
** Sleep for n_micro microseconds. Return the number of microseconds 
** actually slept.
*/
static int __fs_sleep(dbsql_vfs_t *vfs, int n_micro){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->x_sleep(parent, n_micro);
}

/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __fs_current_time(dbsql_vfs_t *vfs, double *time_out){
  dbsql_vfs_t *parent = ((fs_vfs_t *)vfs)->parent;
  return parent->current_time(parent, time_out);
}

/*
** This procedure registers the fs vfs with dbsql. If the argument is
** true, the fs vfs becomes the new default vfs. It is the only publicly
** available function in this file.
*/
int fs_register(void){
  if( fs_vfs.parent ) return DBSQL_OK;
  fs_vfs.parent = dbsql_vfs_find(0);
  fs_vfs.base.mx_pathname = fs_vfs.parent->mx_pathname;
  fs_vfs.base.sz_os_file = MAX(sizeof(tmp_file), sizeof(fs_file));
  return dbsql_vfs_register(&fs_vfs.base, 0);
}

#ifdef DBSQL_TEST
  int DbsqltestOnefile_Init() {return fs_register();}
#endif
