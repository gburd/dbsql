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
** This file contains code for a VFS layer that acts as a wrapper around
** an existing VFS. The code in this file attempts to verify that dbsql
** correctly populates and syncs a journal file before writing to a
** corresponding database file.
**
** INTERFACE
**
**   The public interface to this wrapper VFS is two functions:
**
**     jt_register()
**     jt_unregister()
**
**   See header comments associated with those two functions below for 
**   details.
**
** LIMITATIONS
**
**   This wrapper will not work if "PRAGMA synchronous = off" is used.
**
** OPERATION
**
**  Starting a Transaction:
**
**   When a write-transaction is started, the contents of the database is
**   inspected and the following data stored as part of the database file 
**   handle (type struct jt_file):
**
**     a) The page-size of the database file.
**     b) The number of pages that are in the database file.
**     c) The set of page numbers corresponding to free-list leaf pages.
**     d) A check-sum for every page in the database file.
**
**   The start of a write-transaction is deemed to have occurred when a 
**   28-byte journal header is written to byte offset 0 of the journal 
**   file.
**
**  Syncing the Journal File:
**
**   Whenever the x_sync method is invoked to sync a journal-file, the
**   contents of the journal file are read. For each page written to
**   the journal file, a check-sum is calculated and compared to the  
**   check-sum calculated for the corresponding database page when the
**   write-transaction was initialized. The success of the comparison
**   is assert()ed. So if dbsql has written something other than the
**   original content to the database file, an assert() will fail.
**
**   Additionally, the set of page numbers for which records exist in
**   the journal file is added to (unioned with) the set of page numbers
**   corresponding to free-list leaf pages collected when the 
**   write-transaction was initialized. This set comprises the page-numbers 
**   corresponding to those pages that dbsql may now safely modify.
**
**  Writing to the Database File:
**
**   When a block of data is written to a database file, the following
**   invariants are asserted:
**
**     a) That the block of data is an aligned block of page-size bytes.
**
**     b) That if the page being written did not exist when the 
**        transaction was started (i.e. the database file is growing), then
**        the journal-file must have been synced at least once since
**        the start of the transaction.
**
**     c) That if the page being written did exist when the transaction 
**        was started, then the page must have either been a free-list
**        leaf page at the start of the transaction, or else must have
**        been stored in the journal file prior to the most recent sync.
**
**  Closing a Transaction:
**
**   When a transaction is closed, all data collected at the start of
**   the transaction, or following an x_sync of a journal-file, is 
**   discarded. The end of a transaction is recognized when any one 
**   of the following occur:
**
**     a) A block of zeroes (or anything else that is not a valid 
**        journal-header) is written to the start of the journal file.
**
**     b) A journal file is truncated to zero bytes in size using truncate.
**
**     c) The journal file is deleted using x_delete.
*/
#if DBSQL_TEST          /* This file is used for testing only */

#include "dbsql.h"
#include "dbsql_int.h"

/*
** Maximum pathname length supported by the jt backend.
*/
#define JT_MAX_PATHNAME 512

/*
** Name used to identify this VFS.
*/
#define JT_VFS_NAME "jt"

typedef struct jt_file jt_file;
struct jt_file {
  dbsql_file_t base;
  const char *name;       /* Name of open file */
  int flags;               /* Flags the file was opened with */

  /* The following are only used by database file file handles */
  int lock;               /* Current lock held on the file */
  u_int32_t n_page;               /* Size of file in pages when transaction started */
  u_int32_t n_pagesize;           /* Page size when transaction started */
  bitvec_t *writable;       /* bitvec_t of pages that may be written to the file */
  u_int32_t *cksums;             /* Checksum for first n_page pages */
  int n_sync;               /* Number of times journal file has been synced */

  /* Only used by journal file-handles */
  int64_t i_max_off;   /* Maximum offset written to this transaction */

  jt_file *next;          /* All files are stored in a linked list */
  dbsql_file_t *real;     /* The file handle for the underlying vfs */
};

/*
** Method declarations for jt_file.
*/
static int __jt_close(dbsql_file_t*);
static int __jt_read(dbsql_file_t*, void*, int i_amt, int64_t i_ofst);
static int __jt_write(dbsql_file_t*,const void*,int i_amt, int64_t i_ofst);
static int __jt_truncate(dbsql_file_t*, int64_t size);
static int __jt_sync(dbsql_file_t*, int flags);
static int __jt_file_size(dbsql_file_t*, int64_t *size);
static int __jt_lock(dbsql_file_t*, int);
static int __jt_unlock(dbsql_file_t*, int);
static int __jt_check_reserved_lock(dbsql_file_t*, int *);
static int __jt_file_control(dbsql_file_t*, int op, void *arg);
static int __jt_sector_size(dbsql_file_t*);
static int __jt_device_characteristics(dbsql_file_t*);

/*
** Method declarations for jt_vfs.
*/
static int __jt_open(dbsql_vfs_t*, const char *, dbsql_file_t*, int , int *);
static int __jt_delete(dbsql_vfs_t*, const char *name, int sync_dir);
static int __jt_access(dbsql_vfs_t*, const char *name, int flags, int *);
static int __jt_full_pathname(dbsql_vfs_t*, const char *name, int, char *out);
static void *__jt_dl_open(dbsql_vfs_t*, const char *filename);
static void __jt_dl_error(dbsql_vfs_t*, int n_byte, char *errmsg);
static void (*jt_dl_sym(dbsql_vfs_t*,void*, const char *symbol))(void);
static void __jt_dl_close(dbsql_vfs_t*, void*);
static int __jt_randomness(dbsql_vfs_t*, int n_byte, char *out);
static int __jt_sleep(dbsql_vfs_t*, int microseconds);
static int __jt_current_time(dbsql_vfs_t*, double*);
static int __jt_current_time_int64(dbsql_vfs_t*, int64_t*);
static int __jt_get_last_error(dbsql_vfs_t*, int, char*);

static dbsql_vfs_t jt_vfs = {
  2,                             /* i_version */
  sizeof(jt_file),               /* sz_os_file */
  JT_MAX_PATHNAME,               /* mx_pathname */
  0,                             /* next */
  JT_VFS_NAME,                   /* name */
  0,                             /* app_data */
  __jt_open,                        /* x_open */
  __jt_delete,                      /* x_delete */
  __jt_access,                      /* x_access */
  __jt_full_pathname,                /* full_pathname */
  __jt_dl_open,                      /* dl_open */
  __jt_dl_error,                     /* dl_error */
  jt_dl_sym,                       /* dl_sym */
  __jt_dl_close,                     /* dl_close */
  __jt_randomness,                  /* randomness */
  __jt_sleep,                       /* x_sleep */
  __jt_current_time,                 /* current_time */
  __jt_get_last_error,                /* get_last_error */
  __jt_current_time_int64             /* current_time_int64 */
};

static dbsql_io_methods_t jt_io_methods = {
  1,                             /* i_version */
  __jt_close,                       /* x_close */
  __jt_read,                        /* x_read */
  __jt_write,                       /* x_write */
  __jt_truncate,                    /* truncate */
  __jt_sync,                        /* x_sync */
  __jt_file_size,                    /* file_size */
  __jt_lock,                        /* lock */
  __jt_unlock,                      /* unlock */
  __jt_check_reserved_lock,           /* check_reserved_lock */
  __jt_file_control,                 /* file_control */
  __jt_sector_size,                  /* sector_size */
  __jt_device_characteristics        /* device_characteristics */
};

struct jt_global_t {
  dbsql_vfs_t *vfs;             /* Parent VFS */
  jt_file *list;                /* List of all open files */
};
static struct jt_global_t g = {0, 0};

/*
** Functions to obtain and relinquish a mutex to protect g.list. The
** STATIC_PRNG mutex is reused, purely for the sake of convenience.
*/
static void __enter_jt_mutex(void){
  dbsql_mutex_enter(dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_PRNG));
}
static void __leave_jt_mutex(void){
  dbsql_mutex_leave(dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_PRNG));
}

extern int dbsql_io_error_pending;
extern int dbsql_io_error_hit;
static void stop_ioerr_simulation(int *pi_save, int *pi_save2){
  *pi_save = dbsql_io_error_pending;
  *pi_save2 = dbsql_io_error_hit;
  dbsql_io_error_pending = -1;
  dbsql_io_error_hit = 0;
}
static void start_ioerr_simulation(int i_save, int i_save2){
  dbsql_io_error_pending = i_save;
  dbsql_io_error_hit = i_save2;
}

/*
** The jt_file pointed to by the argument may or may not be a file-handle
** open on a main database file. If it is, and a transaction is currently
** opened on the file, then discard all transaction related data.
*/
static void __close_transaction(jt_file *p){
  __bitvec_destroy(p->writable);
  dbsql_free(p->cksums);
  p->writable = 0;
  p->cksums = 0;
  p->n_sync = 0;
}

/*
** Close an jt-file.
*/
static int __jt_close(dbsql_file_t *file){
  jt_file **pp;
  jt_file *p = (jt_file *)file;

  __close_transaction(p);
  __enter_jt_mutex();
  if( p->name ){
    for(pp=&g.list; *pp!=p; pp=&(*pp)->next);
    *pp = p->next;
  }
  __leave_jt_mutex();
  __dbsql_os_close(p->real);
  return DBSQL_OK;
}

/*
** Read data from an jt-file.
*/
static int __jt_read(
  dbsql_file_t *file, 
  void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  jt_file *p = (jt_file *)file;
  return __dbsql_os_read(p->real, buf, i_amt, i_ofst);
}

/*
** Parameter journal is the name of a journal file that is currently 
** open. This function locates and returns the handle opened on the
** corresponding database file by the pager that currently has the
** journal file opened. This file-handle is identified by the 
** following properties:
**
**   a) DBSQL_OPEN_MAIN_DB was specified when the file was opened.
**
**   b) The file-name specified when the file was opened matches
**      all but the final 8 characters of the journal file name.
**
**   c) There is currently a reserved lock on the file. This 
**      condition is waived if the no_lock argument is non-zero.
**/
static jt_file *__locate_database_handle(const char *journal, int no_lock){
  jt_file *p_main = 0;
  __enter_jt_mutex();
  for(p_main=g.list; p_main; p_main=p_main->next){
    int n_name = (int)(strlen(journal) - strlen("-journal"));
    if( (p_main->flags&DBSQL_OPEN_MAIN_DB)
     && ((int)strlen(p_main->name)==n_name)
     && 0==memcmp(p_main->name, journal, n_name)
     && ((p_main->lock>=DBSQL_LOCK_RESERVED) || no_lock)
    ){
      break;
    }
  }
  __leave_jt_mutex();
  return p_main;
}

/*
** Parameter z points to a buffer of 4 bytes in size containing a 
** unsigned 32-bit integer stored in big-endian format. Decode the 
** integer and return its value.
*/
static u_int32_t __decode_uint32(const unsigned char *z){
  return (z[0]<<24) + (z[1]<<16) + (z[2]<<8) + z[3];
}

/*
** Calculate a checksum from the buffer of length n bytes pointed to
** by parameter z.
*/
static u_int32_t __gen_cksum(const unsigned char *z, int n){
  int i;
  u_int32_t cksum = 0;
  for(i=0; i<n; i++){
    cksum = cksum + z[i] + (cksum<<3);
  }
  return cksum;
}

/*
** The first argument, buf, points to a buffer containing a 28 byte
** serialized journal header. This function deserializes four of the
** integer fields contained in the journal header and writes their
** values to the output variables.
**
** DBSQL_OK is returned if the journal-header is successfully 
** decoded. Otherwise, DBSQL_ERROR.
*/
static int __decode_journal_hdr(
  const unsigned char *buf,         /* Input: 28 byte journal header */
  u_int32_t *pn_rec,                        /* Out: Number of journalled records */
  u_int32_t *pn_page,                       /* Out: Original database page count */
  u_int32_t *pn_sector,                     /* Out: Sector size in bytes */
  u_int32_t *pn_pagesize                    /* Out: Page size in bytes */
){
  unsigned char magics[] = { 0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7 };
  if( memcmp(magics, buf, 8) ) return DBSQL_ERROR;
  if( pn_rec ) *pn_rec = __decode_uint32(&buf[8]);
  if( pn_page ) *pn_page = __decode_uint32(&buf[16]);
  if( pn_sector ) *pn_sector = __decode_uint32(&buf[20]);
  if( pn_pagesize ) *pn_pagesize = __decode_uint32(&buf[24]);
  return DBSQL_OK;
}

/*
** This function is called when a new transaction is opened, just after
** the first journal-header is written to the journal file.
*/
static int __open_transaction(jt_file *p_main, jt_file *journal){
  unsigned char *data;
  dbsql_file_t *p = p_main->real;
  int rc = DBSQL_OK;

  __close_transaction(p_main);
  data = dbsql_malloc(p_main->n_pagesize);
  p_main->writable = __bitvec_create(p_main->n_page);
  p_main->cksums = dbsql_malloc(sizeof(u_int32_t) * (p_main->n_page + 1));
  journal->i_max_off = 0;

  if( !p_main->writable || !p_main->cksums || !data ){
    rc = DBSQL_IOERR_NOMEM;
  }else if( p_main->n_page>0 ){
    u_int32_t i_trunk;
    int i_save;
    int i_save2;

    stop_ioerr_simulation(&i_save, &i_save2);

    /* Read the database free-list. Add the page-number for each free-list
    ** leaf to the jt_file.writable bitvec.
    */
    rc = __dbsql_os_read(p, data, p_main->n_pagesize, 0);
    if( rc==DBSQL_OK ){
      u_int32_t n_dbsize = __decode_uint32(&data[28]);
      if( n_dbsize>0 && memcmp(&data[24], &data[92], 4)==0 ){
        u_int32_t i_pg;
        for(i_pg=n_dbsize+1; i_pg<=p_main->n_page; i_pg++){
          __bitvec_set(p_main->writable, i_pg);
        }
      }
    }
    i_trunk = __decode_uint32(&data[32]);
    while( rc==DBSQL_OK && i_trunk>0 ){
      u_int32_t n_leaf;
      u_int32_t i_leaf;
      int64_t i_off = (int64_t)(i_trunk-1)*p_main->n_pagesize;
      rc = __dbsql_os_read(p, data, p_main->n_pagesize, i_off);
      n_leaf = __decode_uint32(&data[4]);
      for(i_leaf=0; rc==DBSQL_OK && i_leaf<n_leaf; i_leaf++){
        u_int32_t pgno = __decode_uint32(&data[8+4*i_leaf]);
        __bitvec_set(p_main->writable, pgno);
      }
      i_trunk = __decode_uint32(data);
    }

    /* Calculate and store a checksum for each page in the database file. */
    if( rc==DBSQL_OK ){
      int ii;
      for(ii=0; rc==DBSQL_OK && ii<(int)p_main->n_page; ii++){
        int64_t i_off = (int64_t)(p_main->n_pagesize) * (int64_t)ii;
        if( i_off==PENDING_BYTE ) continue;
        rc = __dbsql_os_read(p_main->real, data, p_main->n_pagesize, i_off);
        p_main->cksums[ii] = __gen_cksum(data, p_main->n_pagesize);
        if( ii+1==(int)p_main->n_page && rc==DBSQL_IOERR_SHORT_READ ){
          rc = DBSQL_OK;
        }
      }
    }

    start_ioerr_simulation(i_save, i_save2);
  }

  dbsql_free(data);
  return rc;
}

/*
** The first argument to this function is a handle open on a journal file.
** This function reads the journal file and adds the page number for each
** page in the journal to the bitvec_t object passed as the second argument.
*/
static int __read_journal_file(jt_file *p, jt_file *p_main){
  int rc = DBSQL_OK;
  unsigned char buf[28];
  dbsql_file_t *real = p->real;
  int64_t i_off = 0;
  int64_t i_size = p->i_max_off;
  unsigned char *pages;
  int i_save;
  int i_save2;

  pages = dbsql_malloc(p_main->n_pagesize);
  if( !pages ){
    return DBSQL_IOERR_NOMEM;
  }

  stop_ioerr_simulation(&i_save, &i_save2);

  while( rc==DBSQL_OK && i_off<i_size ){
    u_int32_t n_rec, n_page, n_sector, n_pagesize;
    u_int32_t ii;

    /* Read and decode the next journal-header from the journal file. */
    rc = __dbsql_os_read(real, buf, 28, i_off);
    if( rc!=DBSQL_OK 
     || __decode_journal_hdr(buf, &n_rec, &n_page, &n_sector, &n_pagesize) 
    ){
      goto finish_rjf;
    }
    i_off += n_sector;

    if( n_rec==0 ){
      /* A trick. There might be another journal-header immediately 
      ** following this one. In this case, 0 records means 0 records, 
      ** not "read until the end of the file". See also ticket #2565.
      */
      if( i_size>=(i_off+n_sector) ){
        rc = __dbsql_os_read(real, buf, 28, i_off);
        if( rc!=DBSQL_OK || 0==__decode_journal_hdr(buf, 0, 0, 0, 0) ){
          continue;
        }
      }
      n_rec = (u_int32_t)((i_size-i_off) / (p_main->n_pagesize+8));
    }

    /* Read all the records that follow the journal-header just read. */
    for(ii=0; rc==DBSQL_OK && ii<n_rec && i_off<i_size; ii++){
      u_int32_t pgno;
      rc = __dbsql_os_read(real, buf, 4, i_off);
      if( rc==DBSQL_OK ){
        pgno = __decode_uint32(buf);
        if( pgno>0 && pgno<=p_main->n_page ){
          if( 0==__bitvec_test(p_main->writable, pgno) ){
            rc = __dbsql_os_read(real, pages, p_main->n_pagesize, i_off+4);
            if( rc==DBSQL_OK ){
              u_int32_t cksum = __gen_cksum(pages, p_main->n_pagesize);
              assert( cksum==p_main->cksums[pgno-1] );
            }
          }
          __bitvec_set(p_main->writable, pgno);
        }
        i_off += (8 + p_main->n_pagesize);
      }
    }

    i_off = ((i_off + (n_sector-1)) / n_sector) * n_sector;
  }

finish_rjf:
  start_ioerr_simulation(i_save, i_save2);
  dbsql_free(pages);
  if( rc==DBSQL_IOERR_SHORT_READ ){
    rc = DBSQL_OK;
  }
  return rc;
}

/*
** Write data to an jt-file.
*/
static int __jt_write(
  dbsql_file_t *file, 
  const void *buf, 
  int i_amt, 
  int64_t i_ofst
){
  int rc;
  jt_file *p = (jt_file *)file;
  if( p->flags&DBSQL_OPEN_MAIN_JOURNAL ){
    if( i_ofst==0 ){
      jt_file *p_main = __locate_database_handle(p->name, 0);
      assert( p_main );
  
      if( i_amt==28 ){
        /* Zeroing the first journal-file header. This is the end of a
        ** transaction. */
        __close_transaction(p_main);
      }else if( i_amt!=12 ){
        /* Writing the first journal header to a journal file. This happens
        ** when a transaction is first started.  */
        u_int8_t *z = (u_int8_t *)buf;
        p_main->n_page = __decode_uint32(&z[16]);
        p_main->n_pagesize = __decode_uint32(&z[24]);
        if( DBSQL_OK!=(rc=__open_transaction(p_main, p)) ){
          return rc;
        }
      }
    }
    if( p->i_max_off<(i_ofst + i_amt) ){
      p->i_max_off = i_ofst + i_amt;
    }
  }

  if( p->flags&DBSQL_OPEN_MAIN_DB && p->writable ){
    if( i_amt<(int)p->n_pagesize 
     && p->n_pagesize%i_amt==0 
     && i_ofst>=(PENDING_BYTE+512) 
     && i_ofst+i_amt<=PENDING_BYTE+p->n_pagesize
    ){
      /* No-op. This special case is hit when the backup code is copying a
      ** to a database with a larger page-size than the source database and
      ** it needs to fill in the non-locking-region part of the original
      ** pending-byte page.
      */
    }else{
      u_int32_t pgno = (u_int32_t)(i_ofst/p->n_pagesize + 1);
      assert( (i_amt==1||i_amt==(int)p->n_pagesize) &&
              ((i_ofst+i_amt)%p->n_pagesize)==0 );
      /* The following assert() statements may fail if this layer is used
      ** with a connection in "PRAGMA synchronous=off" mode. If they
      ** fail with sync=normal or sync=full, this may indicate problem.  */
      assert( p->n_page==0 || pgno<=p->n_page || p->n_sync>0 );
      assert( pgno>p->n_page || __bitvec_test(p->writable, pgno) );
    }
  }

  rc = __dbsql_os_write(p->real, buf, i_amt, i_ofst);
  if( (p->flags&DBSQL_OPEN_MAIN_JOURNAL) && i_amt==12 ){
    jt_file *p_main = __locate_database_handle(p->name, 0);
    int rc2 = __read_journal_file(p, p_main);
    if( rc==DBSQL_OK ) rc = rc2;
  }
  return rc;
}

/*
** Truncate an jt-file.
*/
static int __jt_truncate(dbsql_file_t *file, int64_t size){
  jt_file *p = (jt_file *)file;
  if( p->flags&DBSQL_OPEN_MAIN_JOURNAL && size==0 ){
    /* Truncating a journal file. This is the end of a transaction. */
    jt_file *p_main = __locate_database_handle(p->name, 0);
    __close_transaction(p_main);
  }
  if( p->flags&DBSQL_OPEN_MAIN_DB && p->writable ){
    u_int32_t pgno;
    u_int32_t locking_page = (u_int32_t)(PENDING_BYTE/p->n_pagesize+1);
    for(pgno=(u_int32_t)(size/p->n_pagesize+1); pgno<=p->n_page; pgno++){
      assert( pgno==locking_page || __bitvec_test(p->writable, pgno) );
    }
  }
  return __dbsql_os_truncate(p->real, size);
}

/*
** Sync an jt-file.
*/
static int __jt_sync(dbsql_file_t *file, int flags){
  jt_file *p = (jt_file *)file;

  if( p->flags&DBSQL_OPEN_MAIN_JOURNAL ){
    int rc;
    jt_file *p_main;                   /* The associated database file */

    /* The journal file is being synced. At this point, we inspect the 
    ** contents of the file up to this point and set each bit in the 
    ** jt_file.writable bitvec of the main database file associated with
    ** this journal file.
    */
    p_main = __locate_database_handle(p->name, 0);

    /* Set the bitvec values */
    if( p_main && p_main->writable ){
      p_main->n_sync++;
      rc = __read_journal_file(p, p_main);
      if( rc!=DBSQL_OK ){
        return rc;
      }
    }
  }

  return __dbsql_os_sync(p->real, flags);
}

/*
** Return the current file-size of an jt-file.
*/
static int __jt_file_size(dbsql_file_t *file, int64_t *size){
  jt_file *p = (jt_file *)file;
  return __dbsql_os_file_size(p->real, size);
}

/*
** Lock an jt-file.
*/
static int __jt_lock(dbsql_file_t *file, int lock){
  int rc;
  jt_file *p = (jt_file *)file;
  rc = __dbsql_os_lock(p->real, lock);
  if( rc==DBSQL_OK && lock>p->lock ){
    p->lock = lock;
  }
  return rc;
}

/*
** Unlock an jt-file.
*/
static int __jt_unlock(dbsql_file_t *file, int lock){
  int rc;
  jt_file *p = (jt_file *)file;
  rc = __dbsql_os_unlock(p->real, lock);
  if( rc==DBSQL_OK && lock<p->lock ){
    p->lock = lock;
  }
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an jt-file.
*/
static int __jt_check_reserved_lock(dbsql_file_t *file, int *res_out){
  jt_file *p = (jt_file *)file;
  return __dbsql_os_check_reserved_lock(p->real, res_out);
}

/*
** File control method. For custom operations on an jt-file.
*/
static int __jt_file_control(dbsql_file_t *file, int op, void *arg){
  jt_file *p = (jt_file *)file;
  return p->real->methods->file_control(p->real, op, arg);
}

/*
** Return the sector-size in bytes for an jt-file.
*/
static int __jt_sector_size(dbsql_file_t *file){
  jt_file *p = (jt_file *)file;
  return __dbsql_os_sector_size(p->real);
}

/*
** Return the device characteristic flags supported by an jt-file.
*/
static int __jt_device_characteristics(dbsql_file_t *file){
  jt_file *p = (jt_file *)file;
  return __dbsql_os_device_characteristics(p->real);
}

/*
** Open an jt file handle.
*/
static int __jt_open(
  dbsql_vfs_t *vfs,
  const char *name,
  dbsql_file_t *file,
  int flags,
  int *out_flags
){
  int rc;
  jt_file *p = (jt_file *)file;
  file->methods = 0;
  p->real = (dbsql_file_t *)&p[1];
  p->real->methods = 0;
  rc = __dbsql_os_open(g.vfs, name, p->real, flags, out_flags);
  assert( rc==DBSQL_OK || p->real->methods==0 );
  if( rc==DBSQL_OK ){
    file->methods = &jt_io_methods;
    p->lock = 0;
    p->name = name;
    p->flags = flags;
    p->next = 0;
    p->writable = 0;
    p->cksums = 0;
    __enter_jt_mutex();
    if( name ){
      p->next = g.list;
      g.list = p;
    }
    __leave_jt_mutex();
  }
  return rc;
}

/*
** Delete the file located at path. If the dir_sync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int __jt_delete(dbsql_vfs_t *vfs, const char *path, int dir_sync){
  int n_path = (int)strlen(path);
  if( n_path>8 && 0==strcmp("-journal", &path[n_path-8]) ){
    /* Deleting a journal file. The end of a transaction. */
    jt_file *p_main = __locate_database_handle(path, 0);
    if( p_main ){
      __close_transaction(p_main);
    }
  }

  return __dbsql_os_delete(g.vfs, path, dir_sync);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int __jt_access(
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
** of at least (JT_MAX_PATHNAME+1) bytes.
*/
static int __jt_full_pathname(
  dbsql_vfs_t *vfs, 
  const char *path, 
  int n_out, 
  char *out
){
  return __dbsql_os_full_pathname(g.vfs, path, n_out, out);
}

/*
** Open the dynamic library located at path and return a handle.
*/
static void *__jt_dl_open(dbsql_vfs_t *vfs, const char *path){
  return g.vfs->dl_open(g.vfs, path);
}

/*
** Populate the buffer errmsg (size n_byte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void __jt_dl_error(dbsql_vfs_t *vfs, int n_byte, char *errmsg){
  g.vfs->dl_error(g.vfs, n_byte, errmsg);
}

/*
** Return a pointer to the symbol symbol in the dynamic library handle.
*/
static void (*jt_dl_sym(dbsql_vfs_t *vfs, void *p, const char *sym))(void){
  return g.vfs->dl_sym(g.vfs, p, sym);
}

/*
** Close the dynamic library handle handle.
*/
static void __jt_dl_close(dbsql_vfs_t *vfs, void *handle){
  g.vfs->dl_close(g.vfs, handle);
}

/*
** Populate the buffer pointed to by buf_out with n_byte bytes of 
** random data.
*/
static int __jt_randomness(dbsql_vfs_t *vfs, int n_byte, char *buf_out){
  return __dbsql_os_randomness(g.vfs, n_byte, buf_out);
}

/*
** Sleep for n_micro microseconds. Return the number of microseconds 
** actually slept.
*/
static int __jt_sleep(dbsql_vfs_t *vfs, int n_micro){
  return __dbsql_os_sleep(g.vfs, n_micro);
}

/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __jt_current_time(dbsql_vfs_t *vfs, double *time_out){
  return g.vfs->current_time(g.vfs, time_out);
}
/*
** Return the current time as a Julian Day number in *time_out.
*/
static int __jt_current_time_int64(dbsql_vfs_t *vfs, int64_t *time_out){
  return g.vfs->current_time_int64(g.vfs, time_out);
}

static int __jt_get_last_error(dbsql_vfs_t *vfs, int n, char *z){
  return g.vfs->get_last_error(g.vfs, n, z);
}

/**************************************************************************
** Start of public API.
*/

/*
** Configure the jt VFS as a wrapper around the VFS named by parameter 
** wrap. If the is_default parameter is true, then the jt VFS is installed
** as the new default VFS for dbsql connections. If is_default is not
** true, then the jt VFS is installed as non-default. In this case it
** is available via its name, "jt".
*/
int jt_register(char *wrap, int is_default){
  g.vfs = dbsql_vfs_find(wrap);
  if( g.vfs==0 ){
    return DBSQL_ERROR;
  }
  jt_vfs.sz_os_file = sizeof(jt_file) + g.vfs->sz_os_file;
  if( g.vfs->i_version==1 ){
    jt_vfs.i_version = 1;
  }else if( g.vfs->current_time_int64==0 ){
    jt_vfs.current_time_int64 = 0;
  }
  dbsql_vfs_register(&jt_vfs, is_default);
  return DBSQL_OK;
}

/*
** Uninstall the jt VFS, if it is installed.
*/
void jt_unregister(void){
  dbsql_vfs_unregister(&jt_vfs);
}

#endif
