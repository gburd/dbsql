/*
** 2017-12-26
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
** This file implements a virtual table for reading and writing ZIP archive
** files.
**
** Usage example:
**
**     SELECT name, sz, datetime(mtime,'unixepoch') FROM zipfile($filename);
**
** Current limitations:
**
**    *  No support for encryption
**    *  No support for ZIP archives spanning multiple files
**    *  No support for zip64 extensions
**    *  Only the "inflate/deflate" (zlib) compression method is supported
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <stdio.h>
#include <string.h>
#include <assert.h>
#ifndef DBSQL_NO_STDINT
#  include <stdint.h>
#endif

#include <zlib.h>

/* When used as part of the CLI, the dbsql_stdio.h module will have
** been included before this one. In that case use the dbsql_stdio.h
** #defines.  If not, create our own for fopen().
*/
#ifndef _DBSQL_STDIO_H_
# define dbsql_fopen fopen
#endif

#ifndef DBSQL_OMIT_VIRTUALTABLE

#ifndef DBSQL_AMALGAMATION

#ifndef UINT32_TYPE
# ifdef HAVE_UINT32_T
#  define UINT32_TYPE uint32_t
# else
#  define UINT32_TYPE unsigned int
# endif
#endif
#ifndef UINT16_TYPE
# ifdef HAVE_UINT16_T
#  define UINT16_TYPE uint16_t
# else
#  define UINT16_TYPE unsigned short int
# endif
#endif
typedef int64_t int64_t;
typedef unsigned char u_int8_t;
typedef UINT32_TYPE u_int32_t;           /* 4-byte unsigned integer */
typedef UINT16_TYPE u_int16_t;           /* 2-byte unsigned integer */
#define MIN(a,b) ((a)<(b) ? (a) : (b))

#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_MUTATION_TEST)
# define DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS 1
#endif
#if defined(DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif

#endif   /* DBSQL_AMALGAMATION */

/*
** Definitions for mode bitmasks S_IFDIR, S_IFREG and S_IFLNK.
**
** In some ways it would be better to obtain these values from system 
** header files. But, the dependency is undesirable and (a) these
** have been stable for decades, (b) the values are part of POSIX and
** are also made explicit in [man stat], and (c) are part of the 
** file format for zip archives.
*/
#ifndef S_IFDIR
# define S_IFDIR 0040000
#endif
#ifndef S_IFREG
# define S_IFREG 0100000
#endif
#ifndef S_IFLNK
# define S_IFLNK 0120000
#endif

static const char ZIPFILE_SCHEMA[] = 
  "CREATE TABLE y("
    "name PRIMARY KEY,"  /* 0: Name of file in zip archive */
    "mode,"              /* 1: POSIX mode for file */
    "mtime,"             /* 2: Last modification time (secs since 1970)*/
    "sz,"                /* 3: Size of object */
    "rawdata,"           /* 4: Raw data */
    "data,"              /* 5: Uncompressed data */
    "method,"            /* 6: Compression method (integer) */
    "z HIDDEN"           /* 7: Name of zip file */
  ") WITHOUT ROWID;";

#define ZIPFILE_F_COLUMN_IDX 7    /* index_t of column "file" in the above */
#define ZIPFILE_MX_NAME (250)     /* Windows limitation on filename size */

/*
** The buffer should be large enough to contain 3 65536 byte strings - the
** filename, the extra field and the file comment.
*/
#define ZIPFILE_BUFFER_SIZE (200*1024)


/*
** Magic numbers used to read and write zip files.
**
** ZIPFILE_NEWENTRY_MADEBY:
**   Use this value for the "version-made-by" field in new zip file
**   entries. The upper byte indicates "unix", and the lower byte 
**   indicates that the zip file matches pkzip specification 3.0. 
**   This is what info-zip seems to do.
**
** ZIPFILE_NEWENTRY_REQUIRED:
**   Value for "version-required-to-extract" field of new entries.
**   Version 2.0 is required to support folders and deflate compression.
**
** ZIPFILE_NEWENTRY_FLAGS:
**   Value for "general-purpose-bit-flags" field of new entries. Bit
**   11 means "utf-8 filename and comment".
**
** ZIPFILE_SIGNATURE_CDS:
**   First 4 bytes of a valid CDS record.
**
** ZIPFILE_SIGNATURE_LFH:
**   First 4 bytes of a valid LFH record.
**
** ZIPFILE_SIGNATURE_EOCD
**   First 4 bytes of a valid EOCD record.
*/
#define ZIPFILE_EXTRA_TIMESTAMP   0x5455
#define ZIPFILE_NEWENTRY_MADEBY   ((3<<8) + 30)
#define ZIPFILE_NEWENTRY_REQUIRED 20
#define ZIPFILE_NEWENTRY_FLAGS    0x800
#define ZIPFILE_SIGNATURE_CDS     0x02014b50
#define ZIPFILE_SIGNATURE_LFH     0x04034b50
#define ZIPFILE_SIGNATURE_EOCD    0x06054b50

/*
** The sizes of the fixed-size part of each of the three main data 
** structures in a zip archive.
*/
#define ZIPFILE_LFH_FIXED_SZ      30
#define ZIPFILE_EOCD_FIXED_SZ     22
#define ZIPFILE_CDS_FIXED_SZ      46

/*
*** 4.3.16  End of central directory record:
***
***   end of central dir signature    4 bytes  (0x06054b50)
***   number of this disk             2 bytes
***   number of the disk with the
***   start of the central directory  2 bytes
***   total number of entries in the
***   central directory on this disk  2 bytes
***   total number of entries in
***   the central directory           2 bytes
***   size of the central directory   4 bytes
***   offset of start of central
***   directory with respect to
***   the starting disk number        4 bytes
***   .ZIP file comment length        2 bytes
***   .ZIP file comment       (variable size)
*/
typedef struct zipfile_eocd_t zipfile_eocd_t;
struct zipfile_eocd_t {
  u_int16_t i_disk;
  u_int16_t i_first_disk;
  u_int16_t n_entry;
  u_int16_t n_entry_total;
  u_int32_t n_size;
  u_int32_t i_offset;
};

/*
*** 4.3.12  Central directory structure:
***
*** ...
***
***   central file header signature   4 bytes  (0x02014b50)
***   version made by                 2 bytes
***   version needed to extract       2 bytes
***   general purpose bit flag        2 bytes
***   compression method              2 bytes
***   last mod file time              2 bytes
***   last mod file date              2 bytes
***   crc-32                          4 bytes
***   compressed size                 4 bytes
***   uncompressed size               4 bytes
***   file name length                2 bytes
***   extra field length              2 bytes
***   file comment length             2 bytes
***   disk number start               2 bytes
***   internal file attributes        2 bytes
***   external file attributes        4 bytes
***   relative offset of local header 4 bytes
*/
typedef struct zipfile_cds_t zipfile_cds_t;
struct zipfile_cds_t {
  u_int16_t i_version_made_by;
  u_int16_t i_version_extract;
  u_int16_t flags;
  u_int16_t i_compression;
  u_int16_t m_time;
  u_int16_t m_date;
  u_int32_t crc32;
  u_int32_t sz_compressed;
  u_int32_t sz_uncompressed;
  u_int16_t n_file;
  u_int16_t n_extra;
  u_int16_t n_comment;
  u_int16_t i_disk_start;
  u_int16_t i_internal_attr;
  u_int32_t i_external_attr;
  u_int32_t i_offset;
  char *z_file;                    /* Filename (dbsql_malloc()) */
};

/*
*** 4.3.7  Local file header:
***
***   local file header signature     4 bytes  (0x04034b50)
***   version needed to extract       2 bytes
***   general purpose bit flag        2 bytes
***   compression method              2 bytes
***   last mod file time              2 bytes
***   last mod file date              2 bytes
***   crc-32                          4 bytes
***   compressed size                 4 bytes
***   uncompressed size               4 bytes
***   file name length                2 bytes
***   extra field length              2 bytes
***   
*/
typedef struct zipfile_lfh_t zipfile_lfh_t;
struct zipfile_lfh_t {
  u_int16_t i_version_extract;
  u_int16_t flags;
  u_int16_t i_compression;
  u_int16_t m_time;
  u_int16_t m_date;
  u_int32_t crc32;
  u_int32_t sz_compressed;
  u_int32_t sz_uncompressed;
  u_int16_t n_file;
  u_int16_t n_extra;
};

typedef struct zipfile_entry_t zipfile_entry_t;
struct zipfile_entry_t {
  zipfile_cds_t cds;            /* Parsed CDS record */
  u_int32_t m_unix_time;             /* Modification time, in UNIX format */
  u_int8_t *extras;                /* cds.n_extra+cds.n_comment bytes of extra data */
  int64_t i_data_off;              /* Offset to data in file (if data==0) */
  u_int8_t *data;                 /* cds.sz_compressed bytes of compressed data */
  zipfile_entry_t *next;       /* Next element in in-memory CDS */
};

/* 
** Cursor type for zipfile tables.
*/
typedef struct zipfile_csr_t zipfile_csr_t;
struct zipfile_csr_t {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_id;                   /* Cursor ID */
  u_int8_t eof;                   /* True when at EOF */
  u_int8_t noop;                  /* If next next() call is no-op */

  /* Used outside of write transactions */
  FILE *file;               /* Zip file */
  int64_t i_next_off;              /* Offset of next record in central directory */
  zipfile_eocd_t eocd;          /* parser_t of central directory record */

  zipfile_entry_t *free_entry;  /* Free this list when cursor is closed or reset */
  zipfile_entry_t *current;    /* Current entry */
  zipfile_csr_t *csr_next;      /* Next cursor on same virtual table */
};

typedef struct zipfile_tab_t zipfile_tab_t;
struct zipfile_tab_t {
  dbsql_vtab_t base;         /* Base class - must be first */
  char *z_file;               /* Zip file this table accesses (may be NULL) */
  DBSQL *db;               /* Host database connection */
  u_int8_t *buffers;               /* Temporary buffer used for various tasks */

  zipfile_csr_t *csr_list;      /* List of cursors */
  int64_t i_next_csrid;

  /* The following are used by write transactions only */
  zipfile_entry_t *first_entry; /* Linked list of all files (if write_fd!=0) */
  zipfile_entry_t *last_entry;  /* Last element in first_entry list */
  FILE *write_fd;            /* File handle open on zip archive */
  int64_t sz_current;             /* Current size of zip archive */
  int64_t sz_orig;                /* Size of archive at start of transaction */
};

/*
** Set the error message contained in context ctx to the results of
** vprintf(fmt, ...).
*/
static void __zipfile_ctx_error_msg(dbsql_func_t *ctx, const char *fmt, ...){
  char *msg = 0;
  va_list ap;
  va_start(ap, fmt);
  msg = dbsql_vmprintf(fmt, ap);
  dbsql_result_error(ctx, msg, -1);
  dbsql_free(msg);
  va_end(ap);
}

/*
** If string in is quoted, dequote it in place. Otherwise, if the string
** is not quoted, do nothing.
*/
static void __zipfile_dequote(char *in){
  char q = in[0];
  if( q=='"' || q=='\'' || q=='`' || q=='[' ){
    int i_in = 1;
    int i_out = 0;
    if( q=='[' ) q = ']';
    while( ALWAYS(in[i_in]) ){
      char c = in[i_in++];
      if( c==q && in[i_in++]!=q ) break;
      in[i_out++] = c;
    }
    in[i_out] = '\0';
  }
}

/*
** Construct a new zipfile_tab_t virtual table object.
** 
**   argv[0]   -> module name  ("zipfile")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> "column name" and other module argument fields.
*/
static int __zipfile_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  int n_byte = sizeof(zipfile_tab_t) + ZIPFILE_BUFFER_SIZE;
  int n_file = 0;
  const char *file = 0;
  zipfile_tab_t *p_new = 0;
  int rc;
  (void)aux;

  /* If the table name is not "zipfile", require that the argument be
  ** specified. This stops zipfile tables from being created as:
  **
  **   CREATE VIRTUAL TABLE zzz USING zipfile();
  **
  ** It does not prevent:
  **
  **   CREATE VIRTUAL TABLE zipfile USING zipfile();
  */
  assert( 0==dbsql_stricmp(argv[0], "zipfile") );
  if( (0!=dbsql_stricmp(argv[2], "zipfile") && argc<4) || argc>4 ){
    *err_p = dbsql_mprintf("zipfile constructor requires one argument");
    return DBSQL_ERROR;
  }

  if( argc>3 ){
    file = argv[3];
    n_file = (int)strlen(file)+1;
  }

  rc = dbsql_declare_vtab(db, ZIPFILE_SCHEMA);
  if( rc==DBSQL_OK ){
    p_new = (zipfile_tab_t*)dbsql_malloc64((int64_t)n_byte+n_file);
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, n_byte+n_file);
    p_new->db = db;
    p_new->buffers = (u_int8_t*)&p_new[1];
    if( file ){
      p_new->z_file = (char*)&p_new->buffers[ZIPFILE_BUFFER_SIZE];
      memcpy(p_new->z_file, file, n_file);
      __zipfile_dequote(p_new->z_file);
    }
  }
  dbsql_vtab_config(db, DBSQL_VTAB_DIRECTONLY);
  *vtab = (dbsql_vtab_t*)p_new;
  return rc;
}

/*
** Free the zipfile_entry_t structure indicated by the only argument.
*/
static void __zipfile_entry_free(zipfile_entry_t *p){
  if( p ){
    dbsql_free(p->cds.z_file);
    dbsql_free(p);
  }
}

/*
** Release resources that should be freed at the end of a write 
** transaction.
*/
static void __zipfile_cleanup_transaction(zipfile_tab_t *tab){
  zipfile_entry_t *entry;
  zipfile_entry_t *next;

  if( tab->write_fd ){
    fclose(tab->write_fd);
    tab->write_fd = 0;
  }
  for(entry=tab->first_entry; entry; entry=next){
    next = entry->next;
    __zipfile_entry_free(entry);
  }
  tab->first_entry = 0;
  tab->last_entry = 0;
  tab->sz_current = 0;
  tab->sz_orig = 0;
}

/*
** This method is the destructor for zipfile vtab objects.
*/
static int __zipfile_disconnect(dbsql_vtab_t *vtab){
  __zipfile_cleanup_transaction((zipfile_tab_t*)vtab);
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Constructor for a new zipfile_csr_t object.
*/
static int __zipfile_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **pp_csr){
  zipfile_tab_t *tab = (zipfile_tab_t*)p;
  zipfile_csr_t *csr;
  csr = dbsql_malloc64(sizeof(*csr));
  *pp_csr = (dbsql_vtab_cursor_t*)csr;
  if( csr==0 ){
    return DBSQL_NOMEM;
  }
  memset(csr, 0, sizeof(*csr));
  csr->i_id = ++tab->i_next_csrid;
  csr->csr_next = tab->csr_list;
  tab->csr_list = csr;
  return DBSQL_OK;
}

/*
** Reset a cursor back to the state it was in when first returned
** by __zipfile_open().
*/
static void __zipfile_reset_cursor(zipfile_csr_t *csr){
  zipfile_entry_t *p;
  zipfile_entry_t *next;

  csr->eof = 0;
  if( csr->file ){
    fclose(csr->file);
    csr->file = 0;
    __zipfile_entry_free(csr->current);
    csr->current = 0;
  }

  for(p=csr->free_entry; p; p=next){
    next = p->next;
    __zipfile_entry_free(p);
  }
  csr->free_entry = 0;
}

/*
** Destructor for an zipfile_csr_t.
*/
static int __zipfile_close(dbsql_vtab_cursor_t *cur){
  zipfile_csr_t *csr = (zipfile_csr_t*)cur;
  zipfile_tab_t *tab = (zipfile_tab_t*)(csr->base.vtab);
  zipfile_csr_t **pp;
  __zipfile_reset_cursor(csr);

  /* Remove this cursor from the zipfile_tab_t.csr_list list. */
  for(pp=&tab->csr_list; *pp!=csr; pp=&((*pp)->csr_next));
  *pp = csr->csr_next;

  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** Set the error message for the virtual table associated with cursor
** csr to the results of vprintf(fmt, ...).
*/
static void __zipfile_table_err(zipfile_tab_t *tab, const char *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  dbsql_free(tab->base.errmsg);
  tab->base.errmsg = dbsql_vmprintf(fmt, ap);
  va_end(ap);
}
static void __zipfile_cursor_err(zipfile_csr_t *csr, const char *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  dbsql_free(csr->base.vtab->errmsg);
  csr->base.vtab->errmsg = dbsql_vmprintf(fmt, ap);
  va_end(ap);
}

/*
** Read n_read bytes of data from offset i_off of file file into buffer
** reads[]. Return DBSQL_OK if successful, or an dbsql error code
** otherwise. 
**
** If an error does occur, output variable (*errmsg) may be set to point
** to an English language error message. It is the responsibility of the
** caller to eventually free this buffer using
** dbsql_free().
*/
static int __zipfile_read_data(
  FILE *file,                    /* Read from this file */
  u_int8_t *reads,                      /* Read into this buffer */
  int64_t n_read,                      /* Number of bytes to read */
  int64_t i_off,                       /* Offset to read from */
  char **errmsg                 /* OUT: Error message (from dbsql_malloc) */
){
  size_t n;
  fseek(file, (long)i_off, SEEK_SET);
  n = fread(reads, 1, (long)n_read, file);
  if( n!=(size_t)n_read ){
    dbsql_free(*errmsg);
    *errmsg = dbsql_mprintf("error in fread()");
    return DBSQL_ERROR;
  }
  return DBSQL_OK;
}

static int __zipfile_append_data(
  zipfile_tab_t *tab,
  const u_int8_t *writes,
  int n_write
){
  if( n_write>0 ){
    size_t n = n_write;
    fseek(tab->write_fd, (long)tab->sz_current, SEEK_SET);
    n = fwrite(writes, 1, n_write, tab->write_fd);
    if( (int)n!=n_write ){
      __zipfile_table_err(tab,"error in fwrite()");
      return DBSQL_ERROR;
    }
    tab->sz_current += n_write;
  }
  return DBSQL_OK;
}

/*
** Read and return a 16-bit little-endian unsigned integer from buffer bufs.
*/
static u_int16_t __zipfile_get_u16(const u_int8_t *bufs){
  return (bufs[1] << 8) + bufs[0];
}

/*
** Read and return a 32-bit little-endian unsigned integer from buffer bufs.
*/
static u_int32_t __zipfile_get_u32(const u_int8_t *bufs){
  if( bufs==0 ) return 0;
  return ((u_int32_t)(bufs[3]) << 24)
       + ((u_int32_t)(bufs[2]) << 16)
       + ((u_int32_t)(bufs[1]) <<  8)
       + ((u_int32_t)(bufs[0]) <<  0);
}

/*
** Write a 16-bit little endiate integer into buffer bufs.
*/
static void __zipfile_put_u16(u_int8_t *bufs, u_int16_t val){
  bufs[0] = val & 0xFF;
  bufs[1] = (val>>8) & 0xFF;
}

/*
** Write a 32-bit little endiate integer into buffer bufs.
*/
static void __zipfile_put_u32(u_int8_t *bufs, u_int32_t val){
  bufs[0] = val & 0xFF;
  bufs[1] = (val>>8) & 0xFF;
  bufs[2] = (val>>16) & 0xFF;
  bufs[3] = (val>>24) & 0xFF;
}

#define ZIPFILE_READ32(bufs) ( bufs+=4, __zipfile_get_u32(bufs-4) )
#define ZIPFILE_READ16(bufs) ( bufs+=2, __zipfile_get_u16(bufs-2) )

#define ZIPFILE_WRITE32(bufs,val) { __zipfile_put_u32(bufs,val); bufs+=4; }
#define ZIPFILE_WRITE16(bufs,val) { __zipfile_put_u16(bufs,val); bufs+=2; }

/*
** Magic numbers used to read CDS records.
*/
#define ZIPFILE_CDS_NFILE_OFF        28
#define ZIPFILE_CDS_SZCOMPRESSED_OFF 20

/*
** Decode the CDS record in buffer bufs into (*cds). Return DBSQL_ERROR
** if the record is not well-formed, or DBSQL_OK otherwise.
*/
static int __zipfile_read_cds(u_int8_t *bufs, zipfile_cds_t *cds){
  u_int8_t *reads = bufs;
  u_int32_t sig = ZIPFILE_READ32(reads);
  int rc = DBSQL_OK;
  if( sig!=ZIPFILE_SIGNATURE_CDS ){
    rc = DBSQL_ERROR;
  }else{
    cds->i_version_made_by = ZIPFILE_READ16(reads);
    cds->i_version_extract = ZIPFILE_READ16(reads);
    cds->flags = ZIPFILE_READ16(reads);
    cds->i_compression = ZIPFILE_READ16(reads);
    cds->m_time = ZIPFILE_READ16(reads);
    cds->m_date = ZIPFILE_READ16(reads);
    cds->crc32 = ZIPFILE_READ32(reads);
    cds->sz_compressed = ZIPFILE_READ32(reads);
    cds->sz_uncompressed = ZIPFILE_READ32(reads);
    assert( reads==&bufs[ZIPFILE_CDS_NFILE_OFF] );
    cds->n_file = ZIPFILE_READ16(reads);
    cds->n_extra = ZIPFILE_READ16(reads);
    cds->n_comment = ZIPFILE_READ16(reads);
    cds->i_disk_start = ZIPFILE_READ16(reads);
    cds->i_internal_attr = ZIPFILE_READ16(reads);
    cds->i_external_attr = ZIPFILE_READ32(reads);
    cds->i_offset = ZIPFILE_READ32(reads);
    assert( reads==&bufs[ZIPFILE_CDS_FIXED_SZ] );
  }

  return rc;
}

/*
** Decode the LFH record in buffer bufs into (*lfh). Return DBSQL_ERROR
** if the record is not well-formed, or DBSQL_OK otherwise.
*/
static int __zipfile_read_lfh(
  u_int8_t *buffers,
  zipfile_lfh_t *lfh
){
  u_int8_t *reads = buffers;
  int rc = DBSQL_OK;

  u_int32_t sig = ZIPFILE_READ32(reads);
  if( sig!=ZIPFILE_SIGNATURE_LFH ){
    rc = DBSQL_ERROR;
  }else{
    lfh->i_version_extract = ZIPFILE_READ16(reads);
    lfh->flags = ZIPFILE_READ16(reads);
    lfh->i_compression = ZIPFILE_READ16(reads);
    lfh->m_time = ZIPFILE_READ16(reads);
    lfh->m_date = ZIPFILE_READ16(reads);
    lfh->crc32 = ZIPFILE_READ32(reads);
    lfh->sz_compressed = ZIPFILE_READ32(reads);
    lfh->sz_uncompressed = ZIPFILE_READ32(reads);
    lfh->n_file = ZIPFILE_READ16(reads);
    lfh->n_extra = ZIPFILE_READ16(reads);
    if( lfh->n_file>ZIPFILE_MX_NAME ) rc = DBSQL_ERROR;
  }
  return rc;
}


/*
** Buffer extras (size n_extra bytes) contains zip archive "extra" fields.
** Scan through this buffer to find an "extra-timestamp" field. If one
** exists, extract the 32-bit modification-timestamp from it and store
** the value in output parameter *pm_time.
**
** Zero is returned if no extra-timestamp record could be found (and so
** *pm_time is left unchanged), or non-zero otherwise.
**
** The general format of an extra field is:
**
**   Header ID    2 bytes
**   Data Size    2 bytes
**   Data         N bytes
*/
static int __zipfile_scan_extra(u_int8_t *extras, int n_extra, u_int32_t *pm_time){
  int ret = 0;
  u_int8_t *p = extras;
  u_int8_t *end = &extras[n_extra];

  /* Stop when there are less than 9 bytes left to scan in the buffer. This
  ** is because the timestamp field requires exactly 9 bytes - 4 bytes of
  ** header fields and 5 bytes of data. If there are less than 9 bytes 
  ** remaining, either it is some other field or else the extra data
  ** is corrupt. Either way, do not process it.  */
  while( p+(2*sizeof(u_int16_t) + 1 + sizeof(u_int32_t))<=end ){
    u_int16_t id = ZIPFILE_READ16(p);
    u_int16_t n_byte = ZIPFILE_READ16(p);

    switch( id ){
      case ZIPFILE_EXTRA_TIMESTAMP: {
        u_int8_t b = p[0];
        if( b & 0x01 ){     /* 0x01 -> modtime is present */
          *pm_time = __zipfile_get_u32(&p[1]);
          ret = 1;
        }
        break;
      }
    }

    p += n_byte;
  }
  return ret;
}

/*
** Convert the standard MS-DOS timestamp stored in the m_time and m_date
** fields of the CDS structure passed as the only argument to a 32-bit
** UNIX seconds-since-the-epoch timestamp. Return the result.
**
** "Standard" MS-DOS time format:
**
**   File modification time:
**     Bits 00-04: seconds divided by 2
**     Bits 05-10: minute
**     Bits 11-15: hour
**   File modification date:
**     Bits 00-04: day
**     Bits 05-08: month (1-12)
**     Bits 09-15: years from 1980 
**
** https://msdn.microsoft.com/en-us/library/9kkf9tah.aspx
*/
static u_int32_t __zipfile_mtime(zipfile_cds_t *cds){
  int Y,M,D,X1,X2,A,B,sec,min,hr;
  int64_t JDsec;
  Y = (1980 + ((cds->m_date >> 9) & 0x7F));
  M = ((cds->m_date >> 5) & 0x0F);
  D = (cds->m_date & 0x1F);
  sec = (cds->m_time & 0x1F)*2;
  min = (cds->m_time >> 5) & 0x3F;
  hr = (cds->m_time >> 11) & 0x1F;
  if( M<=2 ){
    Y--;
    M += 12;
  }
  X1 = 36525*(Y+4716)/100;
  X2 = 306001*(M+1)/10000;
  A = Y/100;
  B = 2 - A + (A/4);
  JDsec = (int64_t)((X1 + X2 + D + B - 1524.5)*86400) + hr*3600 + min*60 + sec;
  return (u_int32_t)(JDsec - (int64_t)24405875*(int64_t)8640);
}

/*
** The opposite of __zipfile_mtime(). This function populates the m_time and
** m_date fields of the CDS structure passed as the first argument according
** to the UNIX timestamp value passed as the second.
*/
static void __zipfile_mtime_to_dos(zipfile_cds_t *cds, u_int32_t m_unix_time){
  /* Convert unix timestamp to JD (2440588 is noon on 1/1/1970) */
  int64_t JD = (int64_t)2440588 + m_unix_time / (24*60*60);

  int A, B, C, D, E;
  int yr, mon, day;
  int hr, min, sec;

  A = (int)((JD - 1867216.25)/36524.25);
  A = (int)(JD + 1 + A - (A/4));
  B = A + 1524;
  C = (int)((B - 122.1)/365.25);
  D = (36525*(C&32767))/100;
  E = (int)((B-D)/30.6001);

  day = B - D - (int)(30.6001*E);
  mon = (E<14 ? E-1 : E-13);
  yr = mon>2 ? C-4716 : C-4715;

  hr = (m_unix_time % (24*60*60)) / (60*60);
  min = (m_unix_time % (60*60)) / 60;
  sec = (m_unix_time % 60);

  if( yr>=1980 ){
    cds->m_date = (u_int16_t)(day + (mon << 5) + ((yr-1980) << 9));
    cds->m_time = (u_int16_t)(sec/2 + (min<<5) + (hr<<11));
  }else{
    cds->m_date = cds->m_time = 0;
  }

  assert( m_unix_time<315507600 
       || m_unix_time==__zipfile_mtime(cds) 
       || ((m_unix_time % 2) && m_unix_time-1==__zipfile_mtime(cds)) 
       /* || (m_unix_time % 2) */
  );
}

/*
** Set (*err_p) to point to a buffer from dbsql_malloc() containing a 
** generic corruption message and return DBSQL_CORRUPT;
*/
static int __zipfile_corrupt(char **err_p){
  dbsql_free(*err_p);
  *err_p = dbsql_mprintf("zip archive is corrupt");
  return DBSQL_CORRUPT;
}

/*
** If blobs is not NULL, then it is a pointer to a buffer (n_blob bytes in
** size) containing an entire zip archive image. Or, if blobs is NULL,
** then file is a file-handle open on a zip file. In either case, this
** function creates a zipfile_entry_t object based on the zip archive entry
** for which the CDS record is at offset i_off.
**
** If successful, DBSQL_OK is returned and (*entry) set to point to
** the new object. Otherwise, an dbsql error code is returned and the
** final value of (*entry) undefined.
*/
static int __zipfile_get_entry(
  zipfile_tab_t *tab,               /* Store any error message here */
  const u_int8_t *blobs,                /* Pointer to in-memory file image */
  int64_t n_blob,                      /* Size of blobs[] in bytes */
  FILE *file,                    /* If blobs==0, read from this file */
  int64_t i_off,                       /* Offset of CDS record */
  zipfile_entry_t **entry          /* OUT: Pointer to new object */
){
  u_int8_t *reads;
  char **err_p = &tab->base.errmsg;
  int rc = DBSQL_OK;

  if( blobs==0 ){
    reads = tab->buffers;
    rc = __zipfile_read_data(file, reads, ZIPFILE_CDS_FIXED_SZ, i_off, err_p);
  }else{
    if( (i_off+ZIPFILE_CDS_FIXED_SZ)>n_blob ){
      /* Not enough data for the CDS structure. Corruption. */
      return __zipfile_corrupt(err_p);
    }
    reads = (u_int8_t*)&blobs[i_off];
  }

  if( rc==DBSQL_OK ){
    int64_t n_alloc;
    zipfile_entry_t *p_new;

    int n_file = __zipfile_get_u16(&reads[ZIPFILE_CDS_NFILE_OFF]);
    int n_extra = __zipfile_get_u16(&reads[ZIPFILE_CDS_NFILE_OFF+2]);
    n_extra += __zipfile_get_u16(&reads[ZIPFILE_CDS_NFILE_OFF+4]);

    n_alloc = sizeof(zipfile_entry_t) + n_extra;
    if( blobs ){
      n_alloc += __zipfile_get_u32(&reads[ZIPFILE_CDS_SZCOMPRESSED_OFF]);
    }

    p_new = (zipfile_entry_t*)dbsql_malloc64(n_alloc);
    if( p_new==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(p_new, 0, sizeof(zipfile_entry_t));
      rc = __zipfile_read_cds(reads, &p_new->cds);
      if( rc!=DBSQL_OK ){
        __zipfile_table_err(tab, "failed to read CDS at offset %lld", i_off);
      }else if( blobs==0 ){
        rc = __zipfile_read_data(
            file, reads, n_extra+n_file, i_off+ZIPFILE_CDS_FIXED_SZ, err_p
        );
      }else{
        reads = (u_int8_t*)&blobs[i_off + ZIPFILE_CDS_FIXED_SZ];
        if( (i_off + ZIPFILE_CDS_FIXED_SZ + n_file + n_extra)>n_blob ){
          rc = __zipfile_corrupt(err_p);
        }
      }
    }

    if( rc==DBSQL_OK ){
      u_int32_t *pt = &p_new->m_unix_time;
      /* reads[0..n_file-1] might contain embedded \000 characters
      ** See Bug 2026-05-31T11:43:05Z */
      p_new->cds.z_file = dbsql_malloc64(n_file+1);
      if( p_new->cds.z_file!=0 ){
        memcpy(p_new->cds.z_file, reads, n_file);
        p_new->cds.z_file[n_file] = 0;
      }
      p_new->extras = (u_int8_t*)&p_new[1];
      memcpy(p_new->extras, &reads[n_file], n_extra);
      if( p_new->cds.z_file==0 ){
        rc = DBSQL_NOMEM;
      }else if( 0==__zipfile_scan_extra(&reads[n_file], p_new->cds.n_extra, pt) ){
        p_new->m_unix_time = __zipfile_mtime(&p_new->cds);
      }
    }

    if( rc==DBSQL_OK ){
      static const int sz_fix = ZIPFILE_LFH_FIXED_SZ;
      zipfile_lfh_t lfh;
      if( file ){
        rc = __zipfile_read_data(file, reads, sz_fix, p_new->cds.i_offset, err_p);
      }else{
        reads = (u_int8_t*)&blobs[p_new->cds.i_offset];
        if( ((int64_t)p_new->cds.i_offset + ZIPFILE_LFH_FIXED_SZ)>n_blob ){
          rc = __zipfile_corrupt(err_p);
        }
      }

      memset(&lfh, 0, sizeof(lfh));
      if( rc==DBSQL_OK ) rc = __zipfile_read_lfh(reads, &lfh);
      if( rc==DBSQL_OK ){
        p_new->i_data_off =  (int64_t)p_new->cds.i_offset + ZIPFILE_LFH_FIXED_SZ;
        p_new->i_data_off += lfh.n_file + lfh.n_extra;
        if( blobs && p_new->cds.sz_compressed ){
          if( p_new->i_data_off + p_new->cds.sz_compressed > n_blob ){
            rc = __zipfile_corrupt(err_p);
          }else{
            p_new->data = &p_new->extras[n_extra];
            memcpy(p_new->data, &blobs[p_new->i_data_off], p_new->cds.sz_compressed);
          }
        }
      }else{
        __zipfile_table_err(tab, "failed to read LFH at offset %d", 
            (int)p_new->cds.i_offset
        );
      }
    }

    if( rc!=DBSQL_OK ){
      __zipfile_entry_free(p_new);
    }else{
      *entry = p_new;
    }
  }

  return rc;
}

/*
** Advance an zipfile_csr_t to its next row of output.
*/
static int __zipfile_next(dbsql_vtab_cursor_t *cur){
  zipfile_csr_t *csr = (zipfile_csr_t*)cur;
  int rc = DBSQL_OK;

  if( csr->file ){
    int64_t i_eof = (int64_t)csr->eocd.i_offset + (int64_t)csr->eocd.n_size;
    __zipfile_entry_free(csr->current);
    csr->current = 0;
    if( csr->i_next_off>=i_eof ){
      csr->eof = 1;
    }else{
      zipfile_entry_t *p = 0;
      zipfile_tab_t *tab = (zipfile_tab_t*)(cur->vtab);
      rc = __zipfile_get_entry(tab, 0, 0, csr->file, csr->i_next_off, &p);
      if( rc==DBSQL_OK ){
        csr->i_next_off += ZIPFILE_CDS_FIXED_SZ;
        csr->i_next_off += (int)p->cds.n_extra + p->cds.n_file + p->cds.n_comment;
      }
      csr->current = p;
    }
  }else{
    if( !csr->noop ){
      csr->current = csr->current->next;
    }
    if( csr->current==0 ){
      csr->eof = 1;
    }
  }

  csr->noop = 0;
  return rc;
}

static void __zipfile_free(void *p) { 
  dbsql_free(p); 
}

/*
** Buffer ins (size n_in bytes) contains compressed data. Uncompressed, the
** size is n_out bytes. This function uncompresses the data and sets the
** return value in context ctx to the result (a blob).
**
** If an error occurs, an error code is left in ctx instead.
*/
static void __zipfile_inflate(
  dbsql_func_t *ctx,          /* Store result here */
  const u_int8_t *ins,                  /* Compressed data */
  int n_in,                        /* Size of buffer ins[] in bytes */
  int n_out                        /* Expected output size */
){
  u_int8_t *reses = dbsql_malloc64(n_out);
  if( reses==0 ){
    dbsql_result_error_nomem(ctx);
  }else{
    int err;
    z_stream str;
    memset(&str, 0, sizeof(str));

    str.next_in = (Byte*)ins;
    str.avail_in = n_in;
    str.next_out = (Byte*)reses;
    str.avail_out = n_out;

    err = inflateInit2(&str, -15);
    if( err!=Z_OK ){
      __zipfile_ctx_error_msg(ctx, "inflateInit2() failed (%d)", err);
    }else{
      err = inflate(&str, Z_NO_FLUSH);
      if( err!=Z_STREAM_END ){
        __zipfile_ctx_error_msg(ctx, "inflate() failed (%d)", err);
      }else{
        dbsql_result_blob(ctx, reses, (int)str.total_out, __zipfile_free);
        reses = 0;
      }
    }
    dbsql_free(reses);
    inflateEnd(&str);
  }
}

/*
** Buffer ins (size n_in bytes) contains uncompressed data. This function
** compresses it and sets (*out) to point to a buffer containing the
** compressed data. The caller is responsible for eventually calling
** dbsql_free() to release buffer (*out). Before returning, (*pn_out) 
** is set to the size of buffer (*out) in bytes.
**
** If no error occurs, DBSQL_OK is returned. Otherwise, an dbsql error
** code is returned and an error message left in virtual-table handle
** tab. The values of (*out) and (*pn_out) are left unchanged in this
** case.
*/
static int __zipfile_deflate(
  const u_int8_t *ins, int n_in,         /* Input */
  u_int8_t **out, int *pn_out,         /* Output */
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;
  int64_t n_alloc;
  z_stream str;
  u_int8_t *outs;

  memset(&str, 0, sizeof(str));
  str.next_in = (Bytef*)ins;
  str.avail_in = n_in;
  deflateInit2(&str, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);

  n_alloc = deflateBound(&str, n_in);
  outs = (u_int8_t*)dbsql_malloc64(n_alloc);
  if( outs==0 ){
    rc = DBSQL_NOMEM;
  }else{
    int res;
    str.next_out = outs;
    str.avail_out = n_alloc;
    res = deflate(&str, Z_FINISH);
    if( res==Z_STREAM_END ){
      *out = outs;
      *pn_out = (int)str.total_out;
    }else{
      dbsql_free(outs);
      *err_p = dbsql_mprintf("zipfile: deflate() error");
      rc = DBSQL_ERROR;
    }
    deflateEnd(&str);
  }

  return rc;
}


/*
** Return values of columns for the row at which the series_cursor
** is currently pointing.
*/
static int __zipfile_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  zipfile_csr_t *csr = (zipfile_csr_t*)cur;
  zipfile_cds_t *cds = &csr->current->cds;
  int rc = DBSQL_OK;
  switch( i ){
    case 0:   /* name */
      dbsql_result_text(ctx, cds->z_file, -1, DBSQL_TRANSIENT);
      break;
    case 1:   /* mode */
      /* TODO: Whether or not the following is correct surely depends on
      ** the platform on which the archive was created.  */
      dbsql_result_int(ctx, cds->i_external_attr >> 16);
      break;
    case 2: { /* mtime */
      dbsql_result_int64(ctx, csr->current->m_unix_time);
      break;
    }
    case 3: { /* sz */
      if( dbsql_vtab_nochange(ctx)==0 ){
        dbsql_result_int64(ctx, cds->sz_uncompressed);
      }
      break;
    }
    case 4:   /* rawdata */
      if( dbsql_vtab_nochange(ctx) ) break;
    case 5: { /* data */
      if( i==4 || cds->i_compression==0 || cds->i_compression==8 ){
        int sz = cds->sz_compressed;
        int sz_final = cds->sz_uncompressed;
        if( sz_final>0 ){
          u_int8_t *bufs;
          u_int8_t *frees = 0;
          if( csr->current->data ){
            bufs = csr->current->data;
          }else{
            bufs = frees = dbsql_malloc64(sz);
            if( bufs==0 ){
              rc = DBSQL_NOMEM;
            }else{
              FILE *file = csr->file;
              if( file==0 ){
                file = ((zipfile_tab_t*)(csr->base.vtab))->write_fd;
              }
              rc = __zipfile_read_data(file, bufs, sz, csr->current->i_data_off,
                  &csr->base.vtab->errmsg
              );
            }
          }
          if( rc==DBSQL_OK ){
            if( i==5 && cds->i_compression ){
              __zipfile_inflate(ctx, bufs, sz, sz_final);
            }else{
              dbsql_result_blob(ctx, bufs, sz, DBSQL_TRANSIENT);
            }
          }
          dbsql_free(frees);
        }else{
          /* Figure out if this is a directory or a zero-sized file. Consider
          ** it to be a directory either if the mode suggests so, or if
          ** the final character in the name is '/'.  */
          u_int32_t mode = cds->i_external_attr >> 16;
          if( !(mode & S_IFDIR)
           && cds->n_file>=1
           && cds->z_file[cds->n_file-1]!='/'
          ){
            dbsql_result_blob(ctx, "", 0, DBSQL_STATIC);
          }
        }
      }
      break;
    }
    case 6:   /* method */
      dbsql_result_int(ctx, cds->i_compression);
      break;
    default:  /* z */
      assert( i==7 );
      dbsql_result_int64(ctx, csr->i_id);
      break;
  }

  return rc;
}

/*
** Return TRUE if the cursor is at EOF.
*/
static int __zipfile_eof(dbsql_vtab_cursor_t *cur){
  zipfile_csr_t *csr = (zipfile_csr_t*)cur;
  return csr->eof;
}

/*
** If blobs is not NULL, then it points to a buffer n_blob bytes in size
** containing an entire zip archive image. Or, if blobs is NULL, then file
** is guaranteed to be a file-handle open on a zip file.
**
** This function attempts to locate the EOCD record within the zip archive
** and populate *eocd with the results of decoding it. DBSQL_OK is
** returned if successful. Otherwise, an dbsql error code is returned and
** an English language error message may be left in virtual-table tab.
*/
static int __zipfile_read_eocd(
  zipfile_tab_t *tab,               /* Return errors here */
  const u_int8_t *blobs,                /* Pointer to in-memory file image */
  int64_t n_blob,                      /* Size of blobs[] in bytes */
  FILE *file,                    /* Read from this file if blobs==0 */
  zipfile_eocd_t *eocd              /* Object to populate */
){
  u_int8_t *reads = tab->buffers;      /* Temporary buffer */
  int64_t n_read;                      /* Bytes to read from file */
  int rc = DBSQL_OK;

  memset(eocd, 0, sizeof(zipfile_eocd_t));
  if( blobs==0 ){
    int64_t i_off;                     /* Offset to read from */
    int64_t sz_file;                   /* Total size of file in bytes */
    fseek(file, 0, SEEK_END);
    sz_file = (int64_t)ftell(file);
    if( sz_file==0 ){
      return DBSQL_OK;
    }
    n_read = (int)(MIN(sz_file, ZIPFILE_BUFFER_SIZE));
    i_off = sz_file - n_read;
    rc = __zipfile_read_data(file, reads, n_read, i_off, &tab->base.errmsg);
  }else{
    n_read = (int)(MIN(n_blob, ZIPFILE_BUFFER_SIZE));
    reads = (u_int8_t*)&blobs[n_blob-n_read];
  }

  if( rc==DBSQL_OK ){
    int64_t i;

    /* Scan backwards looking for the signature bytes */
    for(i=n_read-20; i>=0; i--){
      if( reads[i]==0x50 && reads[i+1]==0x4b 
       && reads[i+2]==0x05 && reads[i+3]==0x06 
      ){
        break;
      }
    }
    if( i<0 ){
      __zipfile_table_err(tab, "cannot find end of central directory record");
      return DBSQL_ERROR;
    }

    reads += i+4;
    eocd->i_disk = ZIPFILE_READ16(reads);
    eocd->i_first_disk = ZIPFILE_READ16(reads);
    eocd->n_entry = ZIPFILE_READ16(reads);
    eocd->n_entry_total = ZIPFILE_READ16(reads);
    eocd->n_size = ZIPFILE_READ32(reads);
    eocd->i_offset = ZIPFILE_READ32(reads);
  }

  return rc;
}

/*
** Add object p_new to the linked list that begins at zipfile_tab_t.first_entry 
** and ends with last_entry. If argument before is NULL, then p_new is added
** to the end of the list. Otherwise, it is added to the list immediately
** before before (which is guaranteed to be a part of said list).
*/
static void __zipfile_add_entry(
  zipfile_tab_t *tab, 
  zipfile_entry_t *before, 
  zipfile_entry_t *p_new
){
  assert( (tab->first_entry==0)==(tab->last_entry==0) );
  assert( p_new->next==0 );
  if( before==0 ){
    if( tab->first_entry==0 ){
      tab->first_entry = tab->last_entry = p_new;
    }else{
      assert( tab->last_entry->next==0 );
      tab->last_entry->next = p_new;
      tab->last_entry = p_new;
    }
  }else{
    zipfile_entry_t **pp;
    for(pp=&tab->first_entry; *pp!=before; pp=&((*pp)->next));
    p_new->next = before;
    *pp = p_new;
  }
}

static int __zipfile_load_directory(zipfile_tab_t *tab, const u_int8_t *blobs, int64_t n_blob){
  zipfile_eocd_t eocd;
  int rc;
  int i;
  int64_t i_off;

  rc = __zipfile_read_eocd(tab, blobs, n_blob, tab->write_fd, &eocd);
  i_off = eocd.i_offset;
  for(i=0; rc==DBSQL_OK && i<eocd.n_entry; i++){
    zipfile_entry_t *p_new = 0;
    rc = __zipfile_get_entry(tab, blobs, n_blob, tab->write_fd, i_off, &p_new);

    if( rc==DBSQL_OK ){
      __zipfile_add_entry(tab, 0, p_new);
      i_off += ZIPFILE_CDS_FIXED_SZ;
      i_off += (int)p_new->cds.n_extra + p_new->cds.n_file + p_new->cds.n_comment;
    }
  }
  return rc;
}

/*
** filter callback.
*/
static int __zipfile_filter(
  dbsql_vtab_cursor_t *cur, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  zipfile_tab_t *tab = (zipfile_tab_t*)cur->vtab;
  zipfile_csr_t *csr = (zipfile_csr_t*)cur;
  const char *file = 0;          /* Zip file to scan */
  int rc = DBSQL_OK;             /* Return Code */
  int in_memory = 0;              /* True for an in-memory zipfile */

  (void)idx_str;
  (void)argc;

  __zipfile_reset_cursor(csr);

  if( tab->z_file ){
    file = tab->z_file;
  }else if( idx_num==0 ){
    __zipfile_cursor_err(csr, "zipfile() function requires an argument");
    return DBSQL_ERROR;
  }else if( dbsql_value_type(argv[0])==DBSQL_BLOB ){
    static const u_int8_t empty_blobs = 0;
    const u_int8_t *blobs = (const u_int8_t*)dbsql_value_blob(argv[0]);
    int64_t n_blob = dbsql_value_bytes(argv[0]);
    assert( tab->first_entry==0 );
    if( blobs==0 ){
      blobs = &empty_blobs;
      n_blob = 0;
    }
    rc = __zipfile_load_directory(tab, blobs, n_blob);
    csr->free_entry = tab->first_entry;
    tab->first_entry = tab->last_entry = 0;
    if( rc!=DBSQL_OK ) return rc;
    in_memory = 1;
  }else{
    file = (const char*)dbsql_value_text(argv[0]);
  }

  if( 0==tab->write_fd && 0==in_memory ){
    csr->file = file ? dbsql_fopen(file, "rb") : 0;
    if( csr->file==0 ){
      __zipfile_cursor_err(csr, "cannot open file: %s", file);
      rc = DBSQL_ERROR;
    }else{
      rc = __zipfile_read_eocd(tab, 0, 0, csr->file, &csr->eocd);
      if( rc==DBSQL_OK ){
        if( csr->eocd.n_entry==0 ){
          csr->eof = 1;
        }else{
          csr->i_next_off = csr->eocd.i_offset;
          rc = __zipfile_next(cur);
        }
      }
    }
  }else{
    csr->noop = 1;
    csr->current = csr->free_entry ? csr->free_entry : tab->first_entry;
    rc = __zipfile_next(cur);
  }

  return rc;
}

/*
** best_index callback.
*/
static int __zipfile_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;
  int idx = -1;
  int unusable = 0;
  (void)tab;

  for(i=0; i<idx_info->n_constraint; i++){
    const struct dbsql_index_constraint_t *cons = &idx_info->constraints[i];
    if( cons->i_column!=ZIPFILE_F_COLUMN_IDX ) continue;
    if( cons->usable==0 ){
      unusable = 1;
    }else if( cons->op==DBSQL_INDEX_CONSTRAINT_EQ ){
      idx = i;
    }
  }
  idx_info->estimated_cost = 1000.0;
  if( idx>=0 ){
    idx_info->constraint_usages[idx].argv_index = 1;
    idx_info->constraint_usages[idx].omit = 1;
    idx_info->idx_num = 1;
  }else if( unusable ){
    return DBSQL_CONSTRAINT;
  }
  return DBSQL_OK;
}

static zipfile_entry_t *__zipfile_new_entry(const char *path){
  zipfile_entry_t *p_new;
  p_new = dbsql_malloc64(sizeof(zipfile_entry_t));
  if( p_new ){
    memset(p_new, 0, sizeof(zipfile_entry_t));
    p_new->cds.z_file = dbsql_mprintf("%s", path);
    if( p_new->cds.z_file==0 ){
      dbsql_free(p_new);
      p_new = 0;
    }
  }
  return p_new;
}

static int __zipfile_serialize_lfh(zipfile_entry_t *entry, u_int8_t *bufs){
  zipfile_cds_t *cds = &entry->cds;
  u_int8_t *a = bufs;

  cds->n_extra = 9;

  /* Write the LFH itself */
  ZIPFILE_WRITE32(a, ZIPFILE_SIGNATURE_LFH);
  ZIPFILE_WRITE16(a, cds->i_version_extract);
  ZIPFILE_WRITE16(a, cds->flags);
  ZIPFILE_WRITE16(a, cds->i_compression);
  ZIPFILE_WRITE16(a, cds->m_time);
  ZIPFILE_WRITE16(a, cds->m_date);
  ZIPFILE_WRITE32(a, cds->crc32);
  ZIPFILE_WRITE32(a, cds->sz_compressed);
  ZIPFILE_WRITE32(a, cds->sz_uncompressed);
  ZIPFILE_WRITE16(a, (u_int16_t)cds->n_file);
  ZIPFILE_WRITE16(a, cds->n_extra);
  assert( a==&bufs[ZIPFILE_LFH_FIXED_SZ] );

  /* Add the file name */
  memcpy(a, cds->z_file, (int)cds->n_file);
  a += (int)cds->n_file;

  /* The "extra" data */
  ZIPFILE_WRITE16(a, ZIPFILE_EXTRA_TIMESTAMP);
  ZIPFILE_WRITE16(a, 5);
  *a++ = 0x01;
  ZIPFILE_WRITE32(a, entry->m_unix_time);

  return a-bufs;
}

static int __zipfile_append_entry(
  zipfile_tab_t *tab,
  zipfile_entry_t *entry,
  const u_int8_t *data,
  int n_data
){
  u_int8_t *bufs = tab->buffers;
  int n_buf;
  int rc;

  n_buf = __zipfile_serialize_lfh(entry, bufs);
  rc = __zipfile_append_data(tab, bufs, n_buf);
  if( rc==DBSQL_OK ){
    entry->i_data_off = tab->sz_current;
    rc = __zipfile_append_data(tab, data, n_data);
  }

  return rc;
}

static int __zipfile_get_mode(
  dbsql_value_t *val, 
  int is_dir,                     /* If true, default to directory */
  u_int32_t *p_mode,                     /* OUT: shell_mode_t value */
  char **err_p                    /* OUT: Error message */
){
  const char *z = (const char*)dbsql_value_text(val);
  u_int32_t mode = 0;
  if( z==0 ){
    mode = (is_dir ? (S_IFDIR + 0755) : (S_IFREG + 0644));
  }else if( z[0]>='0' && z[0]<='9' ){
    mode = (unsigned int)dbsql_value_int(val);
  }else{
    const char z_template[11] = "-rwxrwxrwx";
    int i;
    if( strlen(z)!=10 ) goto parse_error;
    switch( z[0] ){
      case '-': mode |= S_IFREG; break;
      case 'd': mode |= S_IFDIR; break;
      case 'l': mode |= S_IFLNK; break;
      default: goto parse_error;
    }
    for(i=1; i<10; i++){
      if( z[i]==z_template[i] ) mode |= 1 << (9-i);
      else if( z[i]!='-' ) goto parse_error;
    }
  }
  if( ((mode & S_IFDIR)==0)==is_dir ){
    /* The "mode" attribute is a directory, but data has been specified.
    ** Or vice-versa - no data but "mode" is a file or symlink.  */
    *err_p = dbsql_mprintf("zipfile: mode does not match data");
    return DBSQL_CONSTRAINT;
  }
  *p_mode = mode;
  return DBSQL_OK;

 parse_error:
  *err_p = dbsql_mprintf("zipfile: parse error in mode: %s", z);
  return DBSQL_ERROR;
}

/*
** Both (const char*) arguments point to nul-terminated strings. Argument
** n_b is the value of strlen(b). This function returns 0 if the strings are
** identical, ignoring any trailing '/' character in either path.  */
static int __zipfile_compare_path(const char *a, const char *b, int n_b){
  int n_a = (int)strlen(a);
  if( n_a>0 && a[n_a-1]=='/' ) n_a--;
  if( n_b>0 && b[n_b-1]=='/' ) n_b--;
  if( n_a==n_b && memcmp(a, b, n_a)==0 ) return 0;
  return 1;
}

static int __zipfile_begin(dbsql_vtab_t *vtab){
  zipfile_tab_t *tab = (zipfile_tab_t*)vtab;
  int rc = DBSQL_OK;

  assert( tab->write_fd==0 );
  if( tab->z_file==0 || tab->z_file[0]==0 ){
    __zipfile_table_err(tab, "zipfile: missing filename");
    return DBSQL_ERROR;
  }

  /* Open a write fd on the file. Also load the entire central directory
  ** structure into memory. During the transaction any new file data is 
  ** appended to the archive file, but the central directory is accumulated
  ** in main-memory until the transaction is committed.  */
  tab->write_fd = dbsql_fopen(tab->z_file, "ab+");
  if( tab->write_fd==0 ){
    __zipfile_table_err(tab,
      "zipfile: failed to open file %s for writing", tab->z_file
    );
    rc = DBSQL_ERROR;
  }else{
    fseek(tab->write_fd, 0, SEEK_END);
    tab->sz_current = tab->sz_orig = (int64_t)ftell(tab->write_fd);
    rc = __zipfile_load_directory(tab, 0, 0);
  }

  if( rc!=DBSQL_OK ){
    __zipfile_cleanup_transaction(tab);
  }

  return rc;
}

/*
** Return the current time as a 32-bit timestamp in UNIX epoch format (like
** time(2)).
*/
static u_int32_t __zipfile_time(void){
  dbsql_vfs_t *vfs = dbsql_vfs_find(0);
  u_int32_t ret;
  if( vfs==0 ) return 0;
  if( vfs->i_version>=2 && vfs->current_time_int64 ){
    int64_t ms;
    vfs->current_time_int64(vfs, &ms);
    ret = (u_int32_t)((ms/1000) - ((int64_t)24405875 * 8640));
  }else{
    double day;
    vfs->current_time(vfs, &day);
    ret = (u_int32_t)((day - 2440587.5) * 86400);
  }
  return ret;
}

/*
** Return a 32-bit timestamp in UNIX epoch format.
**
** If the value passed as the only argument is either NULL or an SQL NULL,
** return the current time. Otherwise, return the value stored in (*val)
** cast to a 32-bit unsigned integer.
*/
static u_int32_t __zipfile_get_time(dbsql_value_t *val){
  if( val==0 || dbsql_value_type(val)==DBSQL_NULL ){
    return __zipfile_time();
  }
  return (u_int32_t)dbsql_value_int64(val);
}

/*
** Unless it is NULL, entry old is currently part of the tab->first_entry
** linked list.  Remove it from the list and free the object.
*/
static void __zipfile_remove_entry_from_list(zipfile_tab_t *tab, zipfile_entry_t *old){
  if( old ){
    if( tab->first_entry==old ){
      tab->first_entry = old->next;
      if( tab->last_entry==old ) tab->last_entry = 0;
    }else{
      zipfile_entry_t *p;
      for(p=tab->first_entry; p; p=p->next){
        if( p->next==old ){
          p->next = old->next;
          if( tab->last_entry==old ) tab->last_entry = p;
          break;
        }
      }
    }
    __zipfile_entry_free(old);
  }
}

/*
** update method.
*/
static int __zipfile_update(
  dbsql_vtab_t *vtab, 
  int n_val, 
  dbsql_value_t **vals, 
  int64_t *rowid
){
  zipfile_tab_t *tab = (zipfile_tab_t*)vtab;
  int rc = DBSQL_OK;             /* Return Code */
  zipfile_entry_t *p_new = 0;         /* New in-memory CDS entry */

  u_int32_t mode = 0;                   /* shell_mode_t for new entry */
  u_int32_t m_time = 0;                  /* Modification time for new entry */
  int64_t sz = 0;                     /* Uncompressed size */
  const char *path = 0;          /* Path for new entry */
  int n_path = 0;                  /* strlen(path) */
  const u_int8_t *data = 0;            /* Pointer to buffer containing content */
  int n_data = 0;                  /* Size of data buffer in bytes */
  int i_method = 0;                /* Compression method for new entry */
  u_int8_t *p_free = 0;                  /* Free this */
  char *z_free = 0;                /* Also free this */
  zipfile_entry_t *old = 0;
  zipfile_entry_t *old2 = 0;
  int update = 0;                /* True for an update that modifies "name" */
  int is_dir = 0;
  u_int32_t i_crc32 = 0;

  (void)rowid;

  if( tab->write_fd==0 ){
    rc = __zipfile_begin(vtab);
    if( rc!=DBSQL_OK ) return rc;
  }

  /* If this is a DELETE or UPDATE, find the archive entry to delete. */
  if( dbsql_value_type(vals[0])!=DBSQL_NULL ){
    const char *z_delete = (const char*)dbsql_value_text(vals[0]);
    int n_delete = (int)strlen(z_delete);
    if( n_val>1 ){
      const char *z_update = (const char*)dbsql_value_text(vals[1]);
      if( z_update && __zipfile_compare_path(z_update, z_delete, n_delete)!=0 ){
        update = 1;
      }
    }
    for(old=tab->first_entry; 1; old=old->next){
      if( __zipfile_compare_path(old->cds.z_file, z_delete, n_delete)==0 ){
        break;
      }
      assert( old->next );
    }
  }

  if( n_val>1 ){
    /* Check that "sz" and "rawdata" are both NULL: */
    if( dbsql_value_type(vals[5])!=DBSQL_NULL ){
      __zipfile_table_err(tab, "sz must be NULL");
      rc = DBSQL_CONSTRAINT;
    }
    if( dbsql_value_type(vals[6])!=DBSQL_NULL ){
      __zipfile_table_err(tab, "rawdata must be NULL"); 
      rc = DBSQL_CONSTRAINT;
    }

    if( rc==DBSQL_OK ){
      if( dbsql_value_type(vals[7])==DBSQL_NULL ){
        /* data=NULL. A directory */
        is_dir = 1;
      }else{
        /* Value specified for "data", and possibly "method". This must be
        ** a regular file or a symlink. */
        const u_int8_t *ins = dbsql_value_blob(vals[7]);
        int n_in = dbsql_value_bytes(vals[7]);
        int b_auto = dbsql_value_type(vals[8])==DBSQL_NULL;

        i_method = dbsql_value_int(vals[8]);
        sz = n_in;
        data = ins;
        n_data = n_in;
        if( i_method!=0 && i_method!=8 ){
          __zipfile_table_err(tab, "unknown compression method: %d", i_method);
          rc = DBSQL_CONSTRAINT;
        }else{
          if( b_auto || i_method ){
            int n_cmp;
            rc = __zipfile_deflate(ins, n_in, &p_free, &n_cmp, &tab->base.errmsg);
            if( rc==DBSQL_OK ){
              if( i_method || n_cmp<n_in ){
                i_method = 8;
                data = p_free;
                n_data = n_cmp;
              }
            }
          }
          i_crc32 = crc32(0, ins, n_in);
        }
      }
    }

    if( rc==DBSQL_OK ){
      rc = __zipfile_get_mode(vals[3], is_dir, &mode, &tab->base.errmsg);
    }

    if( rc==DBSQL_OK ){
      path = (const char*)dbsql_value_text(vals[2]);
      if( path==0 ) path = "";
      n_path = (int)strlen(path);
      if( n_path>ZIPFILE_MX_NAME ){
        __zipfile_table_err(tab, "filename too long; max: %d bytes",
                        ZIPFILE_MX_NAME);
        rc = DBSQL_CONSTRAINT;
      }
      m_time = __zipfile_get_time(vals[4]);
    }

    if( rc==DBSQL_OK && is_dir ){
      /* For a directory, check that the last character in the path is a
      ** '/'. This appears to be required for compatibility with info-zip
      ** (the unzip command on unix). It does not create directories
      ** otherwise.  */
      if( n_path<=0 || path[n_path-1]!='/' ){
        z_free = dbsql_mprintf("%s/", path);
        path = (const char*)z_free;
        if( z_free==0 ){
          rc = DBSQL_NOMEM;
          n_path = 0;
        }else{
          n_path = (int)strlen(path);
        }
      }
    }

    /* Check that we're not inserting a duplicate entry -OR- updating an
    ** entry with a path, thereby making it into a duplicate. */
    if( (old==0 || update) && rc==DBSQL_OK ){
      zipfile_entry_t *p;
      for(p=tab->first_entry; p; p=p->next){
        if( __zipfile_compare_path(p->cds.z_file, path, n_path)==0 ){
          switch( dbsql_vtab_on_conflict(tab->db) ){
            case DBSQL_IGNORE: {
              goto zipfile_update_done;
            }
            case DBSQL_REPLACE: {
              old2 = p;
              break;
            }
            default: {
              __zipfile_table_err(tab, "duplicate name: \"%s\"", path);
              rc = DBSQL_CONSTRAINT;
              break;
            }
          }
          break;
        }
      }
    }

    if( rc==DBSQL_OK ){
      /* Create the new CDS record. */
      p_new = __zipfile_new_entry(path);
      if( p_new==0 ){
        rc = DBSQL_NOMEM;
      }else{
        p_new->cds.i_version_made_by = ZIPFILE_NEWENTRY_MADEBY;
        p_new->cds.i_version_extract = ZIPFILE_NEWENTRY_REQUIRED;
        p_new->cds.flags = ZIPFILE_NEWENTRY_FLAGS;
        p_new->cds.i_compression = (u_int16_t)i_method;
        __zipfile_mtime_to_dos(&p_new->cds, m_time);
        p_new->cds.crc32 = i_crc32;
        p_new->cds.sz_compressed = n_data;
        p_new->cds.sz_uncompressed = (u_int32_t)sz;
        p_new->cds.i_external_attr = (mode<<16);
        p_new->cds.i_offset = (u_int32_t)tab->sz_current;
        p_new->cds.n_file = (u_int16_t)n_path;
        p_new->m_unix_time = (u_int32_t)m_time;
        rc = __zipfile_append_entry(tab, p_new, data, n_data);
        __zipfile_add_entry(tab, old, p_new);
      }
    }
  }

  if( rc==DBSQL_OK && (old || old2) ){
    zipfile_csr_t *csr;
    for(csr=tab->csr_list; csr; csr=csr->csr_next){
      if( csr->current && (csr->current==old || csr->current==old2) ){
        csr->current = csr->current->next;
        csr->noop = 1;
      }
    }

    __zipfile_remove_entry_from_list(tab, old);
    __zipfile_remove_entry_from_list(tab, old2);
  }

zipfile_update_done:
  dbsql_free(p_free);
  dbsql_free(z_free);
  return rc;
}

static int __zipfile_serialize_eocd(zipfile_eocd_t *p, u_int8_t *bufs){
  u_int8_t *a = bufs;
  ZIPFILE_WRITE32(a, ZIPFILE_SIGNATURE_EOCD);
  ZIPFILE_WRITE16(a, p->i_disk);
  ZIPFILE_WRITE16(a, p->i_first_disk);
  ZIPFILE_WRITE16(a, p->n_entry);
  ZIPFILE_WRITE16(a, p->n_entry_total);
  ZIPFILE_WRITE32(a, p->n_size);
  ZIPFILE_WRITE32(a, p->i_offset);
  ZIPFILE_WRITE16(a, 0);        /* Size of trailing comment in bytes*/

  return a-bufs;
}

static int __zipfile_append_eocd(zipfile_tab_t *tab, zipfile_eocd_t *p){
  int n_buf = __zipfile_serialize_eocd(p, tab->buffers);
  assert( n_buf==ZIPFILE_EOCD_FIXED_SZ );
  return __zipfile_append_data(tab, tab->buffers, n_buf);
}

/*
** Serialize the CDS structure into buffer bufs[]. Return the number
** of bytes written.
*/
static int __zipfile_serialize_cds(zipfile_entry_t *entry, u_int8_t *bufs){
  u_int8_t *a = bufs;
  zipfile_cds_t *cds = &entry->cds;

  if( entry->extras==0 ){
    cds->n_extra = 9;
  }

  ZIPFILE_WRITE32(a, ZIPFILE_SIGNATURE_CDS);
  ZIPFILE_WRITE16(a, cds->i_version_made_by);
  ZIPFILE_WRITE16(a, cds->i_version_extract);
  ZIPFILE_WRITE16(a, cds->flags);
  ZIPFILE_WRITE16(a, cds->i_compression);
  ZIPFILE_WRITE16(a, cds->m_time);
  ZIPFILE_WRITE16(a, cds->m_date);
  ZIPFILE_WRITE32(a, cds->crc32);
  ZIPFILE_WRITE32(a, cds->sz_compressed);
  ZIPFILE_WRITE32(a, cds->sz_uncompressed);
  assert( a==&bufs[ZIPFILE_CDS_NFILE_OFF] );
  ZIPFILE_WRITE16(a, cds->n_file);
  ZIPFILE_WRITE16(a, cds->n_extra);
  ZIPFILE_WRITE16(a, cds->n_comment);
  ZIPFILE_WRITE16(a, cds->i_disk_start);
  ZIPFILE_WRITE16(a, cds->i_internal_attr);
  ZIPFILE_WRITE32(a, cds->i_external_attr);
  ZIPFILE_WRITE32(a, cds->i_offset);

  memcpy(a, cds->z_file, cds->n_file);
  a += cds->n_file;

  if( entry->extras ){
    int n = (int)cds->n_extra + (int)cds->n_comment;
    memcpy(a, entry->extras, n);
    a += n;
  }else{
    assert( cds->n_extra==9 );
    ZIPFILE_WRITE16(a, ZIPFILE_EXTRA_TIMESTAMP);
    ZIPFILE_WRITE16(a, 5);
    *a++ = 0x01;
    ZIPFILE_WRITE32(a, entry->m_unix_time);
  }

  return a-bufs;
}

static int __zipfile_commit(dbsql_vtab_t *vtab){
  zipfile_tab_t *tab = (zipfile_tab_t*)vtab;
  int rc = DBSQL_OK;
  if( tab->write_fd ){
    int64_t i_offset = tab->sz_current;
    zipfile_entry_t *p;
    zipfile_eocd_t eocd;
    int n_entry = 0;

    /* Write out all entries */
    for(p=tab->first_entry; rc==DBSQL_OK && p; p=p->next){
      int n = __zipfile_serialize_cds(p, tab->buffers);
      rc = __zipfile_append_data(tab, tab->buffers, n);
      n_entry++;
    }

    /* Write out the EOCD record */
    eocd.i_disk = 0;
    eocd.i_first_disk = 0;
    eocd.n_entry = (u_int16_t)n_entry;
    eocd.n_entry_total = (u_int16_t)n_entry;
    eocd.n_size = (u_int32_t)(tab->sz_current - i_offset);
    eocd.i_offset = (u_int32_t)i_offset;
    rc = __zipfile_append_eocd(tab, &eocd);

    __zipfile_cleanup_transaction(tab);
  }
  return rc;
}

static int __zipfile_rollback(dbsql_vtab_t *vtab){
  return __zipfile_commit(vtab);
}

static zipfile_csr_t *__zipfile_find_cursor(zipfile_tab_t *tab, int64_t i_id){
  zipfile_csr_t *csr;
  for(csr=tab->csr_list; csr; csr=csr->csr_next){
    if( i_id==csr->i_id ) break;
  }
  return csr;
}

static void __zipfile_function_cds(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  zipfile_csr_t *csr;
  zipfile_tab_t *tab = (zipfile_tab_t*)dbsql_user_data(context);
  assert( argc>0 );

  csr = __zipfile_find_cursor(tab, dbsql_value_int64(argv[0]));
  if( csr ){
    zipfile_cds_t *p = &csr->current->cds;
    char *res = dbsql_mprintf("{"
        "\"version-made-by\" : %u, "
        "\"version-to-extract\" : %u, "
        "\"flags\" : %u, "
        "\"compression\" : %u, "
        "\"time\" : %u, "
        "\"date\" : %u, "
        "\"crc32\" : %u, "
        "\"compressed-size\" : %u, "
        "\"uncompressed-size\" : %u, "
        "\"file-name-length\" : %u, "
        "\"extra-field-length\" : %u, "
        "\"file-comment-length\" : %u, "
        "\"disk-number-start\" : %u, "
        "\"internal-attr\" : %u, "
        "\"external-attr\" : %u, "
        "\"offset\" : %u }",
        (u_int32_t)p->i_version_made_by, (u_int32_t)p->i_version_extract,
        (u_int32_t)p->flags, (u_int32_t)p->i_compression,
        (u_int32_t)p->m_time, (u_int32_t)p->m_date,
        (u_int32_t)p->crc32, (u_int32_t)p->sz_compressed,
        (u_int32_t)p->sz_uncompressed, (u_int32_t)p->n_file,
        (u_int32_t)p->n_extra, (u_int32_t)p->n_comment,
        (u_int32_t)p->i_disk_start, (u_int32_t)p->i_internal_attr,
        (u_int32_t)p->i_external_attr, (u_int32_t)p->i_offset
    );

    if( res==0 ){
      dbsql_result_error_nomem(context);
    }else{
      dbsql_result_text(context, res, -1, DBSQL_TRANSIENT);
      dbsql_free(res);
    }
  }
}

/*
** find_function method.
*/
static int __zipfile_find_function(
  dbsql_vtab_t *vtab,            /* Virtual table handle */
  int n_arg,                       /* Number of SQL function arguments */
  const char *name,              /* Name of SQL function */
  void (**px_func)(dbsql_func_t*,int,dbsql_value_t**), /* OUT: Result */
  void **arg                    /* OUT: User data for *px_func */
){
  (void)n_arg;
  if( dbsql_stricmp("zipfile_cds", name)==0 ){
    *px_func = __zipfile_function_cds;
    *arg = (void*)vtab;
    return 1;
  }
  return 0;
}

typedef struct zipfile_buffer_t zipfile_buffer_t;
struct zipfile_buffer_t {
  u_int8_t *a;                          /* Pointer to buffer */
  int n;                          /* Size of buffer in bytes */
  int n_alloc;                     /* Byte allocated at a[] */
};

typedef struct zipfile_ctx_t zipfile_ctx_t;
struct zipfile_ctx_t {
  int n_entry;
  zipfile_buffer_t body;
  zipfile_buffer_t cds;
};

static int __zipfile_buffer_grow(zipfile_buffer_t *buf, int64_t n_byte){
  if( (buf->n_alloc-buf->n)<n_byte ){
    u_int8_t *news;
    int64_t n_new = buf->n ? (int64_t)buf->n*2 : 512;
    int64_t n_req = buf->n + n_byte;

    while( n_new<n_req ) n_new = n_new*2;
    news = dbsql_realloc64(buf->a, n_new);
    if( news==0 ) return DBSQL_NOMEM;
    buf->a = news;
    buf->n_alloc = (int)n_new;
  }
  return DBSQL_OK;
}

/*
** step() callback for the zipfile() aggregate. This can be called in
** any of the following ways:
**
**   SELECT zipfile(name,data) ...
**   SELECT zipfile(name,mode,mtime,data) ...
**   SELECT zipfile(name,mode,mtime,data,method) ...
*/
static void __zipfile_step(dbsql_func_t *ctx, int n_val, dbsql_value_t **vals){
  zipfile_ctx_t *p;                  /* Aggregate function context */
  zipfile_entry_t e;                 /* New entry to add to zip archive */

  dbsql_value_t *name = 0;
  dbsql_value_t *p_mode = 0;
  dbsql_value_t *mtime = 0;
  dbsql_value_t *p_data = 0;
  dbsql_value_t *method = 0;

  int is_dir = 0;
  u_int32_t mode;
  int rc = DBSQL_OK;
  char *err = 0;

  int i_method = -1;               /* Compression method to use (0 or 8) */

  const u_int8_t *data = 0;            /* Possibly compressed data for new entry */
  int n_data = 0;                  /* Size of data[] in bytes */
  int sz_uncompressed = 0;         /* Size of data before compression */
  u_int8_t *frees = 0;                  /* Free this before returning */
  u_int32_t i_crc32 = 0;                 /* crc32 of uncompressed data */

  char *z_name = 0;                /* Path (name) of new entry */
  int n_name = 0;                  /* Size of name in bytes */
  char *z_free = 0;                /* Free this before returning */
  int64_t n_byte;

  memset(&e, 0, sizeof(e));
  p = (zipfile_ctx_t*)dbsql_aggregate_context(ctx, sizeof(zipfile_ctx_t));
  if( p==0 ) return;

  /* Martial the arguments into stack variables */
  if( n_val!=2 && n_val!=4 && n_val!=5 ){
    err = dbsql_mprintf("wrong number of arguments to function zipfile()");
    rc = DBSQL_ERROR;
    goto zipfile_step_out;
  }
  name = vals[0];
  if( n_val==2 ){
    p_data = vals[1];
  }else{
    p_mode = vals[1];
    mtime = vals[2];
    p_data = vals[3];
    if( n_val==5 ){
      method = vals[4];
    }
  }

  /* Check that the 'name' parameter looks ok. */
  z_name = (char*)dbsql_value_text(name);
  n_name = dbsql_value_bytes(name);
  if( z_name==0 ){
    err = dbsql_mprintf("first argument to zipfile() must be non-NULL");
    rc = DBSQL_ERROR;
    goto zipfile_step_out;
  }
  if( n_name>ZIPFILE_MX_NAME ){
    err = dbsql_mprintf(
               "filename argument to zipfile() too big; max: %d bytes",
               ZIPFILE_MX_NAME);
    rc = DBSQL_ERROR;
    goto zipfile_step_out;
  }

  /* Inspect the 'method' parameter. This must be either 0 (store), 8 (use
  ** deflate compression) or NULL (choose automatically).  */
  if( method && DBSQL_NULL!=dbsql_value_type(method) ){
    i_method = (int)dbsql_value_int64(method);
    if( i_method!=0 && i_method!=8 ){
      err = dbsql_mprintf("illegal method value: %d", i_method);
      rc = DBSQL_ERROR;
      goto zipfile_step_out;
    }
  }

  /* Now inspect the data. If this is NULL, then the new entry must be a
  ** directory.  Otherwise, figure out whether or not the data should
  ** be deflated or simply stored in the zip archive. */
  if( dbsql_value_type(p_data)==DBSQL_NULL ){
    is_dir = 1;
    i_method = 0;
  }else{
    data = dbsql_value_blob(p_data);
    sz_uncompressed = n_data = dbsql_value_bytes(p_data);
    i_crc32 = crc32(0, data, n_data);
    if( i_method<0 || i_method==8 ){
      int n_out = 0;
      rc = __zipfile_deflate(data, n_data, &frees, &n_out, &err);
      if( rc!=DBSQL_OK ){
        goto zipfile_step_out;
      }
      if( i_method==8 || n_out<n_data ){
        data = frees;
        n_data = n_out;
        i_method = 8;
      }else{
        i_method = 0;
      }
    }
  }

  /* Decode the "mode" argument. */
  rc = __zipfile_get_mode(p_mode, is_dir, &mode, &err);
  if( rc ) goto zipfile_step_out;

  /* Decode the "mtime" argument. */
  e.m_unix_time = __zipfile_get_time(mtime);

  /* If this is a directory entry, ensure that there is exactly one '/'
  ** at the end of the path. Or, if this is not a directory and the path
  ** ends in '/' it is an error. */
  if( is_dir==0 ){
    if( n_name>0 && z_name[n_name-1]=='/' ){
      err = dbsql_mprintf("non-directory name must not end with /");
      rc = DBSQL_ERROR;
      goto zipfile_step_out;
    }
  }else{
    if( n_name==0 || z_name[n_name-1]!='/' ){
      z_name = z_free = dbsql_mprintf("%s/", z_name);
      if( z_name==0 ){
        rc = DBSQL_NOMEM;
        goto zipfile_step_out;
      }
      n_name = (int)strlen(z_name);
    }else{
      while( n_name>1 && z_name[n_name-2]=='/' ) n_name--;
    }
  }

  /* Assemble the zipfile_entry_t object for the new zip archive entry */
  e.cds.i_version_made_by = ZIPFILE_NEWENTRY_MADEBY;
  e.cds.i_version_extract = ZIPFILE_NEWENTRY_REQUIRED;
  e.cds.flags = ZIPFILE_NEWENTRY_FLAGS;
  e.cds.i_compression = (u_int16_t)i_method;
  __zipfile_mtime_to_dos(&e.cds, (u_int32_t)e.m_unix_time);
  e.cds.crc32 = i_crc32;
  e.cds.sz_compressed = n_data;
  e.cds.sz_uncompressed = sz_uncompressed;
  e.cds.i_external_attr = (mode<<16);
  e.cds.i_offset = p->body.n;
  e.cds.n_file = (u_int16_t)n_name;
  e.cds.z_file = z_name;

  /* Append the LFH to the body of the new archive */
  n_byte = ZIPFILE_LFH_FIXED_SZ + e.cds.n_file + 9;
  if( (rc = __zipfile_buffer_grow(&p->body, n_byte)) ) goto zipfile_step_out;
  p->body.n += __zipfile_serialize_lfh(&e, &p->body.a[p->body.n]);

  /* Append the data to the body of the new archive */
  if( n_data>0 ){
    if( (rc = __zipfile_buffer_grow(&p->body, n_data)) ) goto zipfile_step_out;
    memcpy(&p->body.a[p->body.n], data, n_data);
    p->body.n += n_data;
  }

  /* Append the CDS record to the directory of the new archive */
  n_byte = ZIPFILE_CDS_FIXED_SZ + e.cds.n_file + 9;
  if( (rc = __zipfile_buffer_grow(&p->cds, n_byte)) ) goto zipfile_step_out;
  p->cds.n += __zipfile_serialize_cds(&e, &p->cds.a[p->cds.n]);

  /* Increment the count of entries in the archive */
  p->n_entry++;

 zipfile_step_out:
  dbsql_free(frees);
  dbsql_free(z_free);
  if( rc ){
    if( err ){
      dbsql_result_error(ctx, err, -1);
    }else{
      dbsql_result_error_code(ctx, rc);
    }
  }
  dbsql_free(err);
}

/*
** finalize() callback for zipfile aggregate function.
*/
static void __zipfile_final(dbsql_func_t *ctx){
  zipfile_ctx_t *p;
  zipfile_eocd_t eocd;
  int64_t n_zip;
  u_int8_t *zips;

  p = (zipfile_ctx_t*)dbsql_aggregate_context(ctx, sizeof(zipfile_ctx_t));
  if( p==0 ) return;
  if( p->n_entry>0 ){
    memset(&eocd, 0, sizeof(eocd));
    eocd.n_entry = (u_int16_t)p->n_entry;
    eocd.n_entry_total = (u_int16_t)p->n_entry;
    eocd.n_size = p->cds.n;
    eocd.i_offset = p->body.n;

    n_zip = (int64_t)p->body.n + (int64_t)p->cds.n + ZIPFILE_EOCD_FIXED_SZ;
    zips = (u_int8_t*)dbsql_malloc64(n_zip);
    if( zips==0 ){
      dbsql_result_error_nomem(ctx);
    }else{
      memcpy(zips, p->body.a, p->body.n);
      memcpy(&zips[p->body.n], p->cds.a, p->cds.n);
      __zipfile_serialize_eocd(&eocd, &zips[p->body.n + p->cds.n]);
      dbsql_result_blob(ctx, zips, (int)n_zip, __zipfile_free);
    }
  }

  dbsql_free(p->body.a);
  dbsql_free(p->cds.a);
}


/*
** Register the "zipfile" virtual table.
*/
static int ZIPFILE_REGISTER(DBSQL *db){
  static dbsql_module_t zipfile_module = {
    1,                         /* i_version */
    __zipfile_connect,            /* create */
    __zipfile_connect,            /* connect */
    __zipfile_best_index,          /* best_index */
    __zipfile_disconnect,         /* disconnect */
    __zipfile_disconnect,         /* destroy */
    __zipfile_open,               /* x_open - open a cursor */
    __zipfile_close,              /* x_close - close a cursor */
    __zipfile_filter,             /* filter - configure scan constraints */
    __zipfile_next,               /* next - advance a cursor */
    __zipfile_eof,                /* eof - check for end of scan */
    __zipfile_column,             /* column - read data */
    0,                         /* rowid - read data */
    __zipfile_update,             /* update */
    __zipfile_begin,              /* begin */
    0,                         /* x_sync */
    __zipfile_commit,             /* commit */
    __zipfile_rollback,           /* rollback */
    __zipfile_find_function,       /* find_method */
    0,                         /* rename */
    0,                         /* savepoint */
    0,                         /* release */
    0,                         /* rollback */
    0,                         /* shadow_name */
    0                          /* integrity */
  };

  int rc = dbsql_create_module(db, "zipfile"  , &zipfile_module, 0);
  if( rc==DBSQL_OK ) rc = dbsql_overload_function(db, "zipfile_cds", -1);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "zipfile", -1, DBSQL_UTF8, 0, 0, 
        __zipfile_step, __zipfile_final
    );
  }
  assert( sizeof(int64_t)==8 );
  assert( sizeof(u_int32_t)==4 );
  assert( sizeof(u_int16_t)==2 );
  assert( sizeof(u_int8_t)==1 );
  return rc;
}
#else         /* DBSQL_OMIT_VIRTUALTABLE */
# define ZIPFILE_REGISTER(x) DBSQL_OK
#endif

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_zipfile_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  return ZIPFILE_REGISTER(db);
}
