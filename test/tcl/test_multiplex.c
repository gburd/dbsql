/*
** 2010 October 28
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
** This file contains a VFS "shim" - a layer that sits in between the
** pager and the real VFS - that breaks up a very large database file
** into two or more smaller files on disk.  This is useful, for example,
** in order to support large, multi-gigabyte databases on older filesystems
** that limit the maximum file size to 2 __gi_b.
**
** USAGE:
**
** Compile this source file and link it with your application.  Then
** at start-time, invoke the following procedure:
**
**   int dbsql_multiplex_initialize(
**      const char *orig_vfs_name,    // The underlying real VFS
**      int make_default              // True to make multiplex the default VFS
**   );
**
** The procedure call above will create and register a new VFS shim named
** "multiplex".  The multiplex VFS will use the VFS named by orig_vfs_name to
** do the actual disk I/O.  (The orig_vfs_name parameter may be NULL, in
** which case the default VFS at the moment dbsql_multiplex_initialize()
** is called will be used as the underlying real VFS.)
**
** If the make_default parameter is TRUE then multiplex becomes the new
** default VFS.  Otherwise, you can use the multiplex VFS by specifying
** "multiplex" as the 4th parameter to dbsql_open_v2() or by employing
** URI filenames and adding "vfs=multiplex" as a parameter to the filename
** URI.
**
** The multiplex VFS allows databases up to 32 __gi_b in size.  But it splits
** the files up into smaller pieces, so that they will work even on
** filesystems that do not support large files.  The default chunk size
** is 2147418112 bytes (which is 64KiB less than 2GiB) but this can be
** changed at compile-time by defining the DBSQL_MULTIPLEX_CHUNK_SIZE
** macro.  Use the "chunksize=NNNN" query parameter with a URI filename
** in order to select an alternative chunk size for individual connections
** at run-time.
*/
#include "dbsql.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "test_multiplex.h"

#ifndef DBSQL_CORE
  #define DBSQL_CORE 1  /* Disable the API redefinition in sqlite3ext.h */
#endif
#include "dbsql_ext.h"

/*
** These should be defined to be the same as the values in
** dbsql_int.h.  They are defined separately here so that
** the multiplex VFS shim can be built as a loadable
** module.
*/
#define UNUSED_PARAMETER(x) (void)(x)
#define MAX_PAGE_SIZE       0x10000
#define DEFAULT_SECTOR_SIZE 0x1000

/* Maximum chunk number */
#define MX_CHUNK_NUMBER 299

/* First chunk for rollback journal files */
#define DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET 400
#define DBSQL_MULTIPLEX_WAL_8_3_OFFSET 700


/************************ Shim Definitions ******************************/

#ifndef DBSQL_MULTIPLEX_VFS_NAME
# define DBSQL_MULTIPLEX_VFS_NAME "multiplex"
#endif

/* This is the limit on the chunk size.  It may be changed by calling
** the file_control() interface.  It will be rounded up to a
** multiple of MAX_PAGE_SIZE.  We default it here to 2GiB less 64KiB.
*/
#ifndef DBSQL_MULTIPLEX_CHUNK_SIZE
# define DBSQL_MULTIPLEX_CHUNK_SIZE 2147418112
#endif

/* This used to be the default limit on number of chunks, but
** it is no longer enforced. There is currently no limit to the
** number of chunks.
**
** May be changed by calling the file_control() interface.
*/
#ifndef DBSQL_MULTIPLEX_MAX_CHUNKS
# define DBSQL_MULTIPLEX_MAX_CHUNKS 12
#endif

/************************ Object Definitions ******************************/

/* Forward declaration of all object types */
typedef struct multiplex_group multiplex_group;
typedef struct multiplex_conn multiplex_conn;

/*
** A "multiplex group" is a collection of files that collectively
** makeup a single dbsql DB file.  This allows the size of the DB
** to exceed the limits imposed by the file system.
**
** There is an instance of the following object for each defined multiplex
** group.
*/
struct multiplex_group {
  struct multiplex_real {           /* For each chunk */
    dbsql_file_t *p;                  /* Handle for the chunk */
    char *z;                          /* Name of this chunk */
  } *reals;                        /* list of all chunks */
  int n_real;                       /* Number of chunks */
  char *name;                     /* Base filename of this group */
  int n_name;                       /* Length of base filename */
  int flags;                       /* Flags used for original opening */
  unsigned int sz_chunk;            /* Chunk size used for this group */
  unsigned char enabled;          /* TRUE to use Multiplex VFS for this file */
  unsigned char truncate;         /* TRUE to enable truncation of databases */
};

/*
** An instance of the following object represents each open connection
** to a file that is multiplex'ed.  This object is a
** subclass of dbsql_file_t.  The dbsql_file_t object for the underlying
** VFS is appended to this structure.
*/
struct multiplex_conn {
  dbsql_file_t base;              /* Base class - must be first */
  multiplex_group *group;         /* The underlying group of files */
};

/************************* Global Variables **********************************/
/*
** All global variables used by this file are containing within the following
** __g_multiplex structure.
*/
static struct {
  /* The orig_vfs is the real, original underlying VFS implementation.
  ** Most operations pass-through to the real VFS.  This value is read-only
  ** during operation.  It is only modified at start-time and thus does not
  ** require a mutex.
  */
  dbsql_vfs_t *orig_vfs;

  /* The s_this_vfs is the VFS structure used by this shim.  It is initialized
  ** at start-time and thus does not require a mutex
  */
  dbsql_vfs_t s_this_vfs;

  /* The s_io_methods defines the methods used by dbsql_file_t objects
  ** associated with this shim.  It is initialized at start-time and does
  ** not require a mutex.
  **
  ** When the underlying VFS is called to open a file, it might return
  ** either a version 1 or a version 2 dbsql_file_t object.  This shim
  ** has to create a wrapper dbsql_file_t of the same version.  Hence
  ** there are two I/O method structures, one for version 1 and the other
  ** for version 2.
  */
  dbsql_io_methods_t s_io_methods_v1;
  dbsql_io_methods_t s_io_methods_v2;

  /* True when this shim has been initialized.
  */
  int is_initialized;
} __g_multiplex;

/************************* Utility Routines *********************************/
/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
**
** The value returned will never be negative.  Nor will it ever be greater
** than the actual length of the string.  For very long strings (greater
** than 1GiB) the value returned might be less than the true string length.
*/
static int __multiplex_strlen30(const char *z){
  const char *z2 = z;
  if( z==0 ) return 0;
  while( *z2 ){ z2++; }
  return 0x3fffffff & (int)(z2 - z);
}

/*
** Generate the file-name for chunk i_chunk of the group with base name
** base. The file-name is written to buffer out before returning. Buffer
** out must be allocated by the caller so that it is at least (n_base+5)
** bytes in size, where n_base is the length of base, not including the
** nul-terminator.
**
** If i_chunk is 0 (or 400 - the number for the first journal file chunk),
** the output is a copy of the input string. Otherwise, if
** DBSQL_ENABLE_8_3_NAMES is not defined or the input buffer does not contain
** a "." character, then the output is a copy of the input string with the
** three-digit zero-padded decimal representation if i_chunk appended to it.
** For example:
**
**   base="test.db", i_chunk=4  ->  out="test.db004"
**
** Or, if DBSQL_ENABLE_8_3_NAMES is defined and the input buffer contains
** a "." character, then everything after the "." is replaced by the
** three-digit representation of i_chunk.
**
**   base="test.db", i_chunk=4  ->  out="test.004"
**
** The output buffer string is terminated by 2 0x00 bytes. This makes it safe
** to pass to dbsql_uri_parameter() and similar.
*/
static void __multiplex_filename(
  const char *base,              /* Filename for chunk 0 */
  int n_base,                      /* Size of base in bytes (without \0) */
  int flags,                      /* Flags used to open file */
  int i_chunk,                     /* Chunk to generate filename for */
  char *out                      /* Buffer to write generated name to */
){
  int n = n_base;
  memcpy(out, base, n+1);
  if( i_chunk!=0 && i_chunk<=MX_CHUNK_NUMBER ){
#ifdef DBSQL_ENABLE_8_3_NAMES
    int i;
    for(i=n-1; i>0 && i>=n-4 && out[i]!='.'; i--){}
    if( i>=n-4 ) n = i+1;
    if( flags & DBSQL_OPEN_MAIN_JOURNAL ){
      /* The extensions on overflow files for main databases are 001, 002,
      ** 003 and so forth.  To avoid name collisions, add 400 to the
      ** extensions of journal files so that they are 401, 402, 403, ....
      */
      i_chunk += DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET;
    }else if( flags & DBSQL_OPEN_WAL ){
      /* To avoid name collisions, add 700 to the
      ** extensions of WAL files so that they are 701, 702, 703, ....
      */
      i_chunk += DBSQL_MULTIPLEX_WAL_8_3_OFFSET;
    }
#endif
    dbsql_snprintf(4,&out[n],"%03d",i_chunk);
    n += 3;
  }

  assert( out[n]=='\0' );
  out[n+1] = '\0';
}

/* Compute the filename for the i_chunk-th chunk
*/
static int __multiplex_sub_filename(multiplex_group *group, int i_chunk){
  if( i_chunk>=group->n_real ){
    struct multiplex_real *p;
    p = dbsql_realloc64(group->reals, (i_chunk+1)*sizeof(*p));
    if( p==0 ){
      return DBSQL_NOMEM;
    }
    memset(&p[group->n_real], 0, sizeof(p[0])*(i_chunk+1-group->n_real));
    group->reals = p;
    group->n_real = i_chunk+1;
  }
  if( group->name && group->reals[i_chunk].z==0 ){
    char *z;
    int n = group->n_name;
    z = dbsql_malloc64( n+5 );
    if( z==0 ){
      return DBSQL_NOMEM;
    }
    __multiplex_filename(group->name, group->n_name, group->flags, i_chunk, z);
    group->reals[i_chunk].z = (char*)dbsql_create_filename(z,"","",0,0);
    dbsql_free(z);
    if( group->reals[i_chunk].z==0 ) return DBSQL_NOMEM;
  }
  return DBSQL_OK;
}

/* Translate an dbsql_file_t* that is really a multiplex_group* into
** the dbsql_file_t* for the underlying original VFS.
**
** For chunk 0, the group->flags determines whether or not a new file
** is created if it does not already exist.  For chunks 1 and higher, the
** file is created only if create_flag is 1.
*/
static dbsql_file_t *__multiplex_sub_open(
  multiplex_group *group,    /* The multiplexor group */
  int i_chunk,                /* Which chunk to open.  0==original file */
  int *rc,                   /* Result code in and out */
  int *out_flags,            /* Output flags */
  int create_flag             /* True to create if i_chunk>0 */
){
  dbsql_file_t *sub_open = 0;
  dbsql_vfs_t *orig_vfs = __g_multiplex.orig_vfs;        /* Real VFS */

#ifdef DBSQL_ENABLE_8_3_NAMES
  /* If JOURNAL_8_3_OFFSET is set to (say) 400, then any overflow files are
  ** part of a database journal are named db.401, db.402, and so on. A
  ** database may therefore not grow to larger than 400 chunks. Attempting
  ** to open chunk 401 indicates the database is full. */
  if( i_chunk>=DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET ){
    dbsql_log(DBSQL_FULL, "multiplexed chunk overflow: %s", group->name);
    *rc = DBSQL_FULL;
    return 0;
  }
#endif

  *rc = __multiplex_sub_filename(group, i_chunk);
  if( (*rc)==DBSQL_OK && (sub_open = group->reals[i_chunk].p)==0 ){
    int flags, exists;
    flags = group->flags;
    if( create_flag ){
      flags |= DBSQL_OPEN_CREATE;
    }else if( i_chunk==0 ){
      /* Fall through */
    }else if( group->reals[i_chunk].z==0 ){
      return 0;
    }else{
      *rc = orig_vfs->x_access(orig_vfs, group->reals[i_chunk].z,
                              DBSQL_ACCESS_EXISTS, &exists);
     if( *rc || !exists ){
        if( *rc ){
          dbsql_log(*rc, "multiplexor.xAccess failure on %s",
                      group->reals[i_chunk].z);
        }
        return 0;
      }
      flags &= ~DBSQL_OPEN_CREATE;
    }
    sub_open = dbsql_malloc64( orig_vfs->sz_os_file );
    if( sub_open==0 ){
      *rc = DBSQL_IOERR_NOMEM;
      return 0;
    }
    group->reals[i_chunk].p = sub_open;
    *rc = orig_vfs->x_open(orig_vfs, group->reals[i_chunk].z, sub_open,
                          flags, out_flags);
    if( (*rc)!=DBSQL_OK ){
      dbsql_log(*rc, "multiplexor.xOpen failure on %s",
                  group->reals[i_chunk].z);
      dbsql_free(sub_open);
      group->reals[i_chunk].p = 0;
      return 0;
    }
  }
  return sub_open;
}

/*
** Return the size, in bytes, of chunk number i_chunk.  If that chunk
** does not exist, then return 0.  This function does not distinguish between
** non-existent files and zero-length files.
*/
static int64_t __multiplex_sub_size(
  multiplex_group *group,    /* The multiplexor group */
  int i_chunk,                /* Which chunk to open.  0==original file */
  int *rc                    /* Result code in and out */
){
  dbsql_file_t *sub;
  int64_t sz = 0;

  if( *rc ) return 0;
  sub = __multiplex_sub_open(group, i_chunk, rc, NULL, 0);
  if( sub==0 ) return 0;
  *rc = sub->methods->file_size(sub, &sz);
  return sz;
}  

/*
** This is the implementation of the multiplex_control() SQL function.
*/
static void __multiplex_control_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int rc = DBSQL_OK;
  DBSQL *db = dbsql_context_db_handle(context);
  int op = 0;
  int i_val;

  if( !db || argc!=2 ){
    rc = DBSQL_ERROR;
  }else{
    /* extract params */
    op = dbsql_value_int(argv[0]);
    i_val = dbsql_value_int(argv[1]);
    /* map function op to file_control op */
    switch( op ){
      case 1:
        op = MULTIPLEX_CTRL_ENABLE;
        break;
      case 2:
        op = MULTIPLEX_CTRL_SET_CHUNK_SIZE;
        break;
      case 3:
        op = MULTIPLEX_CTRL_SET_MAX_CHUNKS;
        break;
      default:
        rc = DBSQL_NOTFOUND;
        break;
    }
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_file_control(db, 0, op, &i_val);
  }
  dbsql_result_error_code(context, rc);
}

/*
** This is the entry point to register the auto-extension for the
** multiplex_control() function.
*/
static int __multiplex_func_init(
  DBSQL *db,
  char **errmsg_p,
  const dbsql_api_routines_t *api
){
  int rc;
  rc = dbsql_create_function(db, "multiplex_control", 2, DBSQL_ANY,
      0, __multiplex_control_func, 0, 0);
  return rc;
}

/*
** Close a single sub-file in the connection group.
*/
static void __multiplex_sub_close(
  multiplex_group *group,
  int i_chunk,
  dbsql_vfs_t *orig_vfs
){
  dbsql_file_t *sub_open = group->reals[i_chunk].p;
  if( sub_open ){
    sub_open->methods->x_close(sub_open);
    if( orig_vfs && group->reals[i_chunk].z ){
      orig_vfs->x_delete(orig_vfs, group->reals[i_chunk].z, 0);
    }
    dbsql_free(group->reals[i_chunk].p);
  }
  dbsql_free_filename(group->reals[i_chunk].z);
  memset(&group->reals[i_chunk], 0, sizeof(group->reals[i_chunk]));
}

/*
** Deallocate memory held by a multiplex_group
*/
static void __multiplex_free_components(multiplex_group *group){
  int i;
  for(i=0; i<group->n_real; i++){ __multiplex_sub_close(group, i, 0); }
  dbsql_free(group->reals);
  group->reals = 0;
  group->n_real = 0;
}


/************************* VFS Method Wrappers *****************************/

/*
** This is the x_open method used for the "multiplex" VFS.
**
** Most of the work is done by the underlying original VFS.  This method
** simply links the new file into the appropriate multiplex group if it is a
** file that needs to be tracked.
*/
static int __multiplex_open(
  dbsql_vfs_t *vfs,         /* The multiplex VFS */
  const char *name,         /* Name of file to be opened */
  dbsql_file_t *conn,       /* Fill in this file descriptor */
  int flags,                 /* Flags to control the opening */
  int *out_flags             /* Flags showing results of opening */
){
  int rc = DBSQL_OK;                  /* Result code */
  multiplex_conn *multiplex_open;       /* The new multiplex file descriptor */
  multiplex_group *group = 0;          /* Corresponding multiplex_group object */
  dbsql_file_t *sub_open = 0;                    /* Real file descriptor */
  dbsql_vfs_t *orig_vfs = __g_multiplex.orig_vfs;   /* Real VFS */
  int n_name = 0;
  int sz = 0;
  char *to_free = 0;

  UNUSED_PARAMETER(vfs);
  memset(conn, 0, vfs->sz_os_file);
  assert( name || (flags & DBSQL_OPEN_DELETEONCLOSE) );

  /* We need to create a group structure and manage
  ** access to this group of files.
  */
  multiplex_open = (multiplex_conn*)conn;

  if( rc==DBSQL_OK ){
    /* allocate space for group */
    n_name = name ? __multiplex_strlen30(name) : 0;
    sz = sizeof(multiplex_group)                             /* multiplex_group */
       + n_name + 1;                                         /* name */
    group = dbsql_malloc64( sz );
    if( group==0 ){
      rc = DBSQL_NOMEM;
    }
  }

  if( rc==DBSQL_OK ){
    const char *uri = (flags & DBSQL_OPEN_URI) ? name : 0;
    /* assign pointers to extra space allocated */
    memset(group, 0, sz);
    multiplex_open->group = group;
    group->enabled = (unsigned char)-1;
    group->truncate = (unsigned char)dbsql_uri_boolean(uri, "truncate",
                                   (flags & DBSQL_OPEN_MAIN_DB)==0);
    group->sz_chunk = (int)dbsql_uri_int64(uri, "chunksize",
                                        DBSQL_MULTIPLEX_CHUNK_SIZE);
    group->sz_chunk = (group->sz_chunk+0xffff)&~0xffff;
    if( name ){
      char *p = (char *)&group[1];
      group->name = p;
      memcpy(group->name, name, n_name+1);
      group->n_name = n_name;
    }
    if( group->enabled ){
      /* Make sure that the chunksize is such that the pending byte does not
      ** falls at the end of a chunk.  A region of up to 64K following
      ** the pending byte is never written, so if the pending byte occurs
      ** near the end of a chunk, that chunk will be too small. */
#ifndef DBSQL_OMIT_WSD
      extern int __pending_byte;
#else
      int __pending_byte = 0x40000000;
#endif
      while( (__pending_byte % group->sz_chunk)>=(group->sz_chunk-65536) ){
        group->sz_chunk += 65536;
      }
    }
    group->flags = (flags & ~DBSQL_OPEN_URI);
    rc = __multiplex_sub_filename(group, 1);
    if( rc==DBSQL_OK ){
      sub_open = __multiplex_sub_open(group, 0, &rc, out_flags, 0);
      if( sub_open==0 && rc==DBSQL_OK ) rc = DBSQL_CANTOPEN;
    }
    if( rc==DBSQL_OK ){
      int64_t sz64;

      rc = sub_open->methods->file_size(sub_open, &sz64);
      if( rc==DBSQL_OK && name ){
        int exists;
        if( flags & DBSQL_OPEN_SUPER_JOURNAL ){
          group->enabled = 0;
        }else
        if( sz64==0 ){
          if( flags & DBSQL_OPEN_MAIN_JOURNAL ){
            /* If opening a main journal file and the first chunk is zero
            ** bytes in size, delete any subsequent chunks from the
            ** file-system. */
            int i_chunk = 1;
            do {
              rc = orig_vfs->x_access(orig_vfs,
                  group->reals[i_chunk].z, DBSQL_ACCESS_EXISTS, &exists
              );
              if( rc==DBSQL_OK && exists ){
                rc = orig_vfs->x_delete(orig_vfs, group->reals[i_chunk].z, 0);
                if( rc==DBSQL_OK ){
                  rc = __multiplex_sub_filename(group, ++i_chunk);
                }
              }
            }while( rc==DBSQL_OK && exists );
          }
        }else{
          /* If the first overflow file exists and if the size of the main file
          ** is different from the chunk size, that means the chunk size is set
          ** set incorrectly.  So fix it.
          **
          ** Or, if the first overflow file does not exist and the main file is
          ** larger than the chunk size, that means the chunk size is too small.
          ** But we have no way of determining the intended chunk size, so
          ** just disable the multiplexor all together.
          */
          rc = orig_vfs->x_access(orig_vfs, group->reals[1].z,
              DBSQL_ACCESS_EXISTS, &exists);
          exists = __multiplex_sub_size(group, 1, &rc)>0;
          if( rc==DBSQL_OK && exists && sz64==(sz64&0xffff0000) && sz64>0
              && sz64!=group->sz_chunk ){
            group->sz_chunk = (int)sz64;
          }else if( rc==DBSQL_OK && !exists && sz64>group->sz_chunk ){
            group->enabled = 0;
          }
        }
      }
    }

    if( rc==DBSQL_OK ){
      if( sub_open->methods->i_version==1 ){
        conn->methods = &__g_multiplex.s_io_methods_v1;
      }else{
        conn->methods = &__g_multiplex.s_io_methods_v2;
      }
    }else{
      __multiplex_free_components(group);
      dbsql_free(group);
    }
  }
  dbsql_free(to_free);
  return rc;
}

/*
** This is the x_delete method used for the "multiplex" VFS.
** It attempts to delete the filename specified.
*/
static int __multiplex_delete(
  dbsql_vfs_t *vfs,         /* The multiplex VFS */
  const char *name,         /* Name of file to delete */
  int sync_dir
){
  int rc;
  dbsql_vfs_t *orig_vfs = __g_multiplex.orig_vfs;   /* Real VFS */
  rc = orig_vfs->x_delete(orig_vfs, name, sync_dir);
  if( rc==DBSQL_OK ){
    /* If the main chunk was deleted successfully, also delete any subsequent
    ** chunks - starting with the last (highest numbered).
    */
    int n_name = (int)strlen(name);
    char *z;
    z = dbsql_malloc64(n_name + 5);
    if( z==0 ){
      rc = DBSQL_IOERR_NOMEM;
    }else{
      int i_chunk = 0;
      int exists;
      do{
        __multiplex_filename(name, n_name, DBSQL_OPEN_MAIN_JOURNAL, ++i_chunk, z);
        rc = orig_vfs->x_access(orig_vfs, z, DBSQL_ACCESS_EXISTS, &exists);
      }while( rc==DBSQL_OK && exists );
      while( rc==DBSQL_OK && i_chunk>1 ){
        __multiplex_filename(name, n_name, DBSQL_OPEN_MAIN_JOURNAL, --i_chunk, z);
        rc = orig_vfs->x_delete(orig_vfs, z, sync_dir);
      }
      if( rc==DBSQL_OK ){
        i_chunk = 0;
        do{
          __multiplex_filename(name, n_name, DBSQL_OPEN_WAL, ++i_chunk, z);
          rc = orig_vfs->x_access(orig_vfs, z, DBSQL_ACCESS_EXISTS, &exists);
        }while( rc==DBSQL_OK && exists );
        while( rc==DBSQL_OK && i_chunk>1 ){
          __multiplex_filename(name, n_name, DBSQL_OPEN_WAL, --i_chunk, z);
          rc = orig_vfs->x_delete(orig_vfs, z, sync_dir);
        }
      }
    }
    dbsql_free(z);
  }
  return rc;
}

static int __multiplex_access(dbsql_vfs_t *a, const char *b, int c, int *d){
  return __g_multiplex.orig_vfs->x_access(__g_multiplex.orig_vfs, b, c, d);
}
static int __multiplex_full_pathname(dbsql_vfs_t *a, const char *b, int c, char *d){
  return __g_multiplex.orig_vfs->full_pathname(__g_multiplex.orig_vfs, b, c, d);
}
static void *__multiplex_dl_open(dbsql_vfs_t *a, const char *b){
  return __g_multiplex.orig_vfs->dl_open(__g_multiplex.orig_vfs, b);
}
static void __multiplex_dl_error(dbsql_vfs_t *a, int b, char *c){
  __g_multiplex.orig_vfs->dl_error(__g_multiplex.orig_vfs, b, c);
}
static void (*multiplex_dl_sym(dbsql_vfs_t *a, void *b, const char *c))(void){
  return __g_multiplex.orig_vfs->dl_sym(__g_multiplex.orig_vfs, b, c);
}
static void __multiplex_dl_close(dbsql_vfs_t *a, void *b){
  __g_multiplex.orig_vfs->dl_close(__g_multiplex.orig_vfs, b);
}
static int __multiplex_randomness(dbsql_vfs_t *a, int b, char *c){
  return __g_multiplex.orig_vfs->randomness(__g_multiplex.orig_vfs, b, c);
}
static int __multiplex_sleep(dbsql_vfs_t *a, int b){
  return __g_multiplex.orig_vfs->x_sleep(__g_multiplex.orig_vfs, b);
}
static int __multiplex_current_time(dbsql_vfs_t *a, double *b){
  return __g_multiplex.orig_vfs->current_time(__g_multiplex.orig_vfs, b);
}
static int __multiplex_get_last_error(dbsql_vfs_t *a, int b, char *c){
  if( __g_multiplex.orig_vfs->get_last_error ){
    return __g_multiplex.orig_vfs->get_last_error(__g_multiplex.orig_vfs, b, c);
  }else{
    return 0;
  }
}
static int __multiplex_current_time_int64(dbsql_vfs_t *a, int64_t *b){
  return __g_multiplex.orig_vfs->current_time_int64(__g_multiplex.orig_vfs, b);
}

/************************ I/O Method Wrappers *******************************/

/* x_close requests get passed through to the original VFS.
** We loop over all open chunk handles and close them.
** The group structure for this file is unlinked from
** our list of groups and freed.
*/
static int __multiplex_close(dbsql_file_t *conn){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  __multiplex_free_components(group);
  dbsql_free(group);
  return rc;
}

/* Pass x_read requests thru to the original VFS after
** determining the correct chunk to operate on.
** Break up reads across chunk boundaries.
*/
static int __multiplex_read(
  dbsql_file_t *conn,
  void *buf,
  int i_amt,
  int64_t i_ofst
){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  if( !group->enabled ){
    dbsql_file_t *sub_open = __multiplex_sub_open(group, 0, &rc, NULL, 0);
    if( sub_open==0 ){
      rc = DBSQL_IOERR_READ;
    }else{
      rc = sub_open->methods->x_read(sub_open, buf, i_amt, i_ofst);
    }
  }else{
    while( i_amt > 0 ){
      int i = (int)(i_ofst / group->sz_chunk);
      dbsql_file_t *sub_open;
      sub_open = __multiplex_sub_open(group, i, &rc, NULL, 1);
      if( sub_open ){
        int extra = ((int)(i_ofst % group->sz_chunk) + i_amt) - group->sz_chunk;
        if( extra<0 ) extra = 0;
        i_amt -= extra;
        rc = sub_open->methods->x_read(sub_open, buf, i_amt,
                                       i_ofst % group->sz_chunk);
        if( rc!=DBSQL_OK ) break;
        buf = (char *)buf + i_amt;
        i_ofst += i_amt;
        i_amt = extra;
      }else{
        rc = DBSQL_IOERR_READ;
        break;
      }
    }
  }

  return rc;
}

/* Pass x_write requests thru to the original VFS after
** determining the correct chunk to operate on.
** Break up writes across chunk boundaries.
*/
static int __multiplex_write(
  dbsql_file_t *conn,
  const void *buf,
  int i_amt,
  int64_t i_ofst
){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  if( !group->enabled ){
    dbsql_file_t *sub_open = __multiplex_sub_open(group, 0, &rc, NULL, 0);
    if( sub_open==0 ){
      rc = DBSQL_IOERR_WRITE;
    }else{
      rc = sub_open->methods->x_write(sub_open, buf, i_amt, i_ofst);
    }
  }else{
    while( rc==DBSQL_OK && i_amt>0 ){
      int i = (int)(i_ofst / group->sz_chunk);
      dbsql_file_t *sub_open = __multiplex_sub_open(group, i, &rc, NULL, 1);
      if( sub_open ){
        int extra = ((int)(i_ofst % group->sz_chunk) + i_amt) -
                    group->sz_chunk;
        if( extra<0 ) extra = 0;
        i_amt -= extra;
        rc = sub_open->methods->x_write(sub_open, buf, i_amt,
                                        i_ofst % group->sz_chunk);
        buf = (char *)buf + i_amt;
        i_ofst += i_amt;
        i_amt = extra;
      }
    }
  }
  return rc;
}

/* Pass truncate requests thru to the original VFS after
** determining the correct chunk to operate on.  Delete any
** chunks above the truncate mark.
*/
static int __multiplex_truncate(dbsql_file_t *conn, int64_t size){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  if( !group->enabled ){
    dbsql_file_t *sub_open = __multiplex_sub_open(group, 0, &rc, NULL, 0);
    if( sub_open==0 ){
      rc = DBSQL_IOERR_TRUNCATE;
    }else{
      rc = sub_open->methods->truncate(sub_open, size);
    }
  }else{
    int i;
    int i_base_group = (int)(size / group->sz_chunk);
    dbsql_file_t *sub_open;
    dbsql_vfs_t *orig_vfs = __g_multiplex.orig_vfs;   /* Real VFS */
    /* delete the chunks above the truncate limit */
    for(i = group->n_real-1; i>i_base_group && rc==DBSQL_OK; i--){
      if( group->truncate ){
        __multiplex_sub_close(group, i, orig_vfs);
      }else{
        sub_open = __multiplex_sub_open(group, i, &rc, 0, 0);
        if( sub_open ){
          rc = sub_open->methods->truncate(sub_open, 0);
        }
      }
    }
    if( rc==DBSQL_OK ){
      sub_open = __multiplex_sub_open(group, i_base_group, &rc, 0, 0);
      if( sub_open ){
        rc = sub_open->methods->truncate(sub_open, size % group->sz_chunk);
      }
    }
    if( rc ) rc = DBSQL_IOERR_TRUNCATE;
  }
  return rc;
}

/* Pass x_sync requests through to the original VFS without change
*/
static int __multiplex_sync(dbsql_file_t *conn, int flags){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  int i;
  for(i=0; i<group->n_real; i++){
    dbsql_file_t *sub_open = group->reals[i].p;
    if( sub_open ){
      int rc2 = sub_open->methods->x_sync(sub_open, flags);
      if( rc2!=DBSQL_OK ) rc = rc2;
    }
  }
  return rc;
}

/* Pass file_size requests through to the original VFS.
** Aggregate the size of all the chunks before returning.
*/
static int __multiplex_file_size(dbsql_file_t *conn, int64_t *size){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_OK;
  int i;
  if( !group->enabled ){
    dbsql_file_t *sub_open = __multiplex_sub_open(group, 0, &rc, NULL, 0);
    if( sub_open==0 ){
      rc = DBSQL_IOERR_FSTAT;
    }else{
      rc = sub_open->methods->file_size(sub_open, size);
    }
  }else{
    *size = 0;
    for(i=0; rc==DBSQL_OK; i++){
      int64_t sz = __multiplex_sub_size(group, i, &rc);
      if( sz==0 ) break;
      *size = i*(int64_t)group->sz_chunk + sz;
    }
  }
  return rc;
}

/* Pass lock requests through to the original VFS unchanged.
*/
static int __multiplex_lock(dbsql_file_t *conn, int lock){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->lock(sub_open, lock);
  }
  return DBSQL_BUSY;
}

/* Pass unlock requests through to the original VFS unchanged.
*/
static int __multiplex_unlock(dbsql_file_t *conn, int lock){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->unlock(sub_open, lock);
  }
  return DBSQL_IOERR_UNLOCK;
}

/* Pass check_reserved_lock requests through to the original VFS unchanged.
*/
static int __multiplex_check_reserved_lock(dbsql_file_t *conn, int *res_out){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->check_reserved_lock(sub_open, res_out);
  }
  return DBSQL_IOERR_CHECKRESERVEDLOCK;
}

/* Pass file_control requests through to the original VFS unchanged,
** except for any MULTIPLEX_CTRL_* requests here.
*/
static int __multiplex_file_control(dbsql_file_t *conn, int op, void *arg){
  multiplex_conn *p = (multiplex_conn*)conn;
  multiplex_group *group = p->group;
  int rc = DBSQL_ERROR;
  dbsql_file_t *sub_open;

  if( !__g_multiplex.is_initialized ) return DBSQL_MISUSE;
  switch( op ){
    case MULTIPLEX_CTRL_ENABLE:
      if( arg ) {
        int enabled = *(int *)arg;
        group->enabled = (unsigned char)enabled;
        rc = DBSQL_OK;
      }
      break;
    case MULTIPLEX_CTRL_SET_CHUNK_SIZE:
      if( arg ) {
        unsigned int sz_chunk = *(unsigned*)arg;
        if( sz_chunk<1 ){
          rc = DBSQL_MISUSE;
        }else{
          /* Round up to nearest multiple of MAX_PAGE_SIZE. */
          sz_chunk = (sz_chunk + (MAX_PAGE_SIZE-1));
          sz_chunk &= ~(MAX_PAGE_SIZE-1);
          group->sz_chunk = sz_chunk;
          rc = DBSQL_OK;
        }
      }
      break;
    case MULTIPLEX_CTRL_SET_MAX_CHUNKS:
      rc = DBSQL_OK;
      break;
    case DBSQL_FCNTL_SIZE_HINT:
    case DBSQL_FCNTL_CHUNK_SIZE:
      /* no-op these */
      rc = DBSQL_OK;
      break;
    case DBSQL_FCNTL_PRAGMA: {
      char **fcntls = (char**)arg;
      /*
      ** EVIDENCE-OF: R-29875-31678 The argument to the DBSQL_FCNTL_PRAGMA
      ** file control is an array of pointers to strings (char**) in which the
      ** second element of the array is the name of the pragma and the third
      ** element is the argument to the pragma or NULL if the pragma has no
      ** argument.
      */
      if( fcntls[1] && dbsql_strnicmp(fcntls[1],"multiplex_",10)==0 ){
        int64_t sz = 0;
        (void)__multiplex_file_size(conn, &sz);
        /*
        ** PRAGMA multiplex_truncate=BOOLEAN;
        ** PRAGMA multiplex_truncate;
        **
        ** Turn the multiplexor truncate feature on or off.  Return either
        ** "on" or "off" to indicate the new setting.  If the BOOLEAN argument
        ** is omitted, just return the current value for the truncate setting.
        */
        if( dbsql_stricmp(fcntls[1],"multiplex_truncate")==0 ){
          if( fcntls[2] && fcntls[2][0] ){
            if( dbsql_stricmp(fcntls[2], "on")==0
             || dbsql_stricmp(fcntls[2], "1")==0 ){
              group->truncate = 1;
            }else
            if( dbsql_stricmp(fcntls[2], "off")==0
             || dbsql_stricmp(fcntls[2], "0")==0 ){
              group->truncate = 0;
            }
          }
          /* EVIDENCE-OF: R-27806-26076 The handler for an DBSQL_FCNTL_PRAGMA
          ** file control can optionally make the first element of the char**
          ** argument point to a string obtained from dbsql_mprintf() or the
          ** equivalent and that string will become the result of the pragma
          ** or the error message if the pragma fails.
          */
          fcntls[0] = dbsql_mprintf(group->truncate ? "on" : "off");
          rc = DBSQL_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_enabled;
        **
        ** Return 0 or 1 depending on whether the multiplexor is enabled or
        ** disabled, respectively.
        */
        if( dbsql_stricmp(fcntls[1],"multiplex_enabled")==0 ){
          fcntls[0] = dbsql_mprintf("%d", group->enabled!=0);
          rc = DBSQL_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_chunksize;
        **
        ** Return the chunksize for the multiplexor, or no-op if the
        ** multiplexor is not active.
        */
        if( dbsql_stricmp(fcntls[1],"multiplex_chunksize")==0
         && group->enabled
        ){
          fcntls[0] = dbsql_mprintf("%u", group->sz_chunk);
          rc = DBSQL_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_filecount;
        **
        ** Return the number of disk files currently in use by the
        ** multiplexor.  This should be the total database size size
        ** divided by the chunksize and rounded up.
        */
        if( dbsql_stricmp(fcntls[1],"multiplex_filecount")==0 ){
          int n = 0;
          int ii;
          for(ii=0; ii<group->n_real; ii++){
            if( group->reals[ii].p!=0 ) n++;
          }
          fcntls[0] = dbsql_mprintf("%d", n);
          rc = DBSQL_OK;
          break;
        }
      }
      /* If the multiplexor does not handle the pragma, pass it through
      ** into the default case. */
    }
    default:
      sub_open = __multiplex_sub_open(group, 0, &rc, NULL, 0);
      if( sub_open ){
        rc = sub_open->methods->file_control(sub_open, op, arg);
        if( op==DBSQL_FCNTL_VFSNAME && rc==DBSQL_OK ){
         *(char**)arg = dbsql_mprintf("multiplex/%z", *(char**)arg);
        }
      }
      break;
  }
  return rc;
}

/* Pass sector_size requests through to the original VFS unchanged.
*/
static int __multiplex_sector_size(dbsql_file_t *conn){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open && sub_open->methods->sector_size ){
    return sub_open->methods->sector_size(sub_open);
  }
  return DEFAULT_SECTOR_SIZE;
}

/* Pass device_characteristics requests through to the original VFS unchanged.
*/
static int __multiplex_device_characteristics(dbsql_file_t *conn){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->device_characteristics(sub_open);
  }
  return 0;
}

/* Pass shm_map requests through to the original VFS unchanged.
*/
static int __multiplex_shm_map(
  dbsql_file_t *conn,            /* Handle open on database file */
  int i_region,                    /* Region to retrieve */
  int sz_region,                   /* Size of regions */
  int extend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->shm_map(sub_open, i_region, sz_region, extend,pp);
  }
  return DBSQL_IOERR;
}

/* Pass shm_lock requests through to the original VFS unchanged.
*/
static int __multiplex_shm_lock(
  dbsql_file_t *conn,       /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->shm_lock(sub_open, ofst, n, flags);
  }
  return DBSQL_BUSY;
}

/* Pass shm_barrier requests through to the original VFS unchanged.
*/
static void __multiplex_shm_barrier(dbsql_file_t *conn){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    sub_open->methods->shm_barrier(sub_open);
  }
}

/* Pass shm_unmap requests through to the original VFS unchanged.
*/
static int __multiplex_shm_unmap(dbsql_file_t *conn, int delete_flag){
  multiplex_conn *p = (multiplex_conn*)conn;
  int rc;
  dbsql_file_t *sub_open = __multiplex_sub_open(p->group, 0, &rc, NULL, 0);
  if( sub_open ){
    return sub_open->methods->shm_unmap(sub_open, delete_flag);
  }
  return DBSQL_OK;
}

/************************** Public Interfaces *****************************/
/*
** CAPI: Initialize the multiplex VFS shim - dbsql_multiplex_initialize()
**
** Use the VFS named orig_vfs_name as the VFS that does the actual work.
** Use the default if orig_vfs_name==NULL.
**
** The multiplex VFS shim is named "multiplex".  It will become the default
** VFS if make_default is non-zero.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once
** during start-up.
*/
int dbsql_multiplex_initialize(const char *orig_vfs_name, int make_default){
  dbsql_vfs_t *orig_vfs;
  if( __g_multiplex.is_initialized ) return DBSQL_MISUSE;
  orig_vfs = dbsql_vfs_find(orig_vfs_name);
  if( orig_vfs==0 ) return DBSQL_ERROR;
  assert( orig_vfs!=&__g_multiplex.s_this_vfs );
  __g_multiplex.is_initialized = 1;
  __g_multiplex.orig_vfs = orig_vfs;
  __g_multiplex.s_this_vfs = *orig_vfs;
  __g_multiplex.s_this_vfs.sz_os_file += sizeof(multiplex_conn);
  __g_multiplex.s_this_vfs.name = DBSQL_MULTIPLEX_VFS_NAME;
  __g_multiplex.s_this_vfs.x_open = __multiplex_open;
  __g_multiplex.s_this_vfs.x_delete = __multiplex_delete;
  __g_multiplex.s_this_vfs.x_access = __multiplex_access;
  __g_multiplex.s_this_vfs.full_pathname = __multiplex_full_pathname;
  __g_multiplex.s_this_vfs.dl_open = __multiplex_dl_open;
  __g_multiplex.s_this_vfs.dl_error = __multiplex_dl_error;
  __g_multiplex.s_this_vfs.dl_sym = multiplex_dl_sym;
  __g_multiplex.s_this_vfs.dl_close = __multiplex_dl_close;
  __g_multiplex.s_this_vfs.randomness = __multiplex_randomness;
  __g_multiplex.s_this_vfs.x_sleep = __multiplex_sleep;
  __g_multiplex.s_this_vfs.current_time = __multiplex_current_time;
  __g_multiplex.s_this_vfs.get_last_error = __multiplex_get_last_error;
  __g_multiplex.s_this_vfs.current_time_int64 = __multiplex_current_time_int64;

  __g_multiplex.s_io_methods_v1.i_version = 1;
  __g_multiplex.s_io_methods_v1.x_close = __multiplex_close;
  __g_multiplex.s_io_methods_v1.x_read = __multiplex_read;
  __g_multiplex.s_io_methods_v1.x_write = __multiplex_write;
  __g_multiplex.s_io_methods_v1.truncate = __multiplex_truncate;
  __g_multiplex.s_io_methods_v1.x_sync = __multiplex_sync;
  __g_multiplex.s_io_methods_v1.file_size = __multiplex_file_size;
  __g_multiplex.s_io_methods_v1.lock = __multiplex_lock;
  __g_multiplex.s_io_methods_v1.unlock = __multiplex_unlock;
  __g_multiplex.s_io_methods_v1.check_reserved_lock = __multiplex_check_reserved_lock;
  __g_multiplex.s_io_methods_v1.file_control = __multiplex_file_control;
  __g_multiplex.s_io_methods_v1.sector_size = __multiplex_sector_size;
  __g_multiplex.s_io_methods_v1.device_characteristics =
                                            __multiplex_device_characteristics;
  __g_multiplex.s_io_methods_v2 = __g_multiplex.s_io_methods_v1;
  __g_multiplex.s_io_methods_v2.i_version = 2;
  __g_multiplex.s_io_methods_v2.shm_map = __multiplex_shm_map;
  __g_multiplex.s_io_methods_v2.shm_lock = __multiplex_shm_lock;
  __g_multiplex.s_io_methods_v2.shm_barrier = __multiplex_shm_barrier;
  __g_multiplex.s_io_methods_v2.shm_unmap = __multiplex_shm_unmap;
  dbsql_vfs_register(&__g_multiplex.s_this_vfs, make_default);

  dbsql_auto_extension((void(*)(void))__multiplex_func_init);

  return DBSQL_OK;
}

/*
** CAPI: Shutdown the multiplex system - dbsql_multiplex_shutdown()
**
** All dbsql database connections must be closed before calling this
** routine.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once while
** shutting down in order to free all remaining multiplex groups.
*/
int dbsql_multiplex_shutdown(int force){
  int rc = DBSQL_OK;
  if( __g_multiplex.is_initialized==0 ) return DBSQL_MISUSE;
  __g_multiplex.is_initialized = 0;
  dbsql_vfs_unregister(&__g_multiplex.s_this_vfs);
  memset(&__g_multiplex, 0, sizeof(__g_multiplex));
  return rc;
}

/***************************** Test Code ***********************************/
#ifdef DBSQL_TEST
#include "tcl_dbsql.h"
extern const char *__err_name(int);


/*
** tclcmd: dbsql_multiplex_initialize NAME MAKEDEFAULT
*/
static int DBSQL_TCLAPI test_multiplex_initialize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *name;              /* Name of new multiplex VFS */
  int make_default;                /* True to make the new VFS the default */
  int rc;                         /* Value returned by multiplex_initialize() */

  UNUSED_PARAMETER(clientData);

  /* Process arguments */
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME MAKEDEFAULT");
    return TCL_ERROR;
  }
  name = Tcl_GetString(objv[1]);
  if( Tcl_GetBooleanFromObj(interp, objv[2], &make_default) ) return TCL_ERROR;
  if( name[0]=='\0' ) name = 0;

  /* Call dbsql_multiplex_initialize() */
  rc = dbsql_multiplex_initialize(name, make_default);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** tclcmd: dbsql_multiplex_shutdown
*/
static int DBSQL_TCLAPI test_multiplex_shutdown(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Value returned by multiplex_shutdown() */

  UNUSED_PARAMETER(clientData);

  if( objc==2 && strcmp(Tcl_GetString(objv[1]),"-force")!=0 ){
    objc = 3;
  }
  if( (objc!=1 && objc!=2) ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-force?");
    return TCL_ERROR;
  }

  /* Call dbsql_multiplex_shutdown() */
  rc = dbsql_multiplex_shutdown(objc==2);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** Tclcmd: test_multiplex_control HANDLE DBNAME SUB-COMMAND ?INT-VALUE?
*/
static int DBSQL_TCLAPI test_multiplex_control(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Return code from file_control() */
  int idx;                        /* index_t in subs[] */
  Tcl_CmdInfo cmd_info;            /* Command info structure for HANDLE */
  DBSQL *db;                    /* Underlying db handle for HANDLE */
  int i_value = 0;
  void *arg = 0;

  struct sub_command_t {
    const char *name;
    int op;
    int argtype;
  } subs[] = {
    { "enable",       MULTIPLEX_CTRL_ENABLE,           1 },
    { "chunk_size",   MULTIPLEX_CTRL_SET_CHUNK_SIZE,   1 },
    { "max_chunks",   MULTIPLEX_CTRL_SET_MAX_CHUNKS,   1 },
    { 0, 0, 0 }
  };

  if( objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE DBNAME SUB-COMMAND INT-VALUE");
    return TCL_ERROR;
  }

  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(objv[1]), &cmd_info) ){
    Tcl_AppendResult(interp, "expected database handle, got \"", NULL);
    Tcl_AppendResult(interp, Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }else{
    db = *(DBSQL **)cmd_info.objClientData;
  }

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[3], subs, sizeof(subs[0]), "sub-command", 0, &idx
  );
  if( rc!=TCL_OK ) return rc;

  switch( subs[idx].argtype ){
    case 1:
      if( Tcl_GetIntFromObj(interp, objv[4], &i_value) ){
        return TCL_ERROR;
      }
      arg = (void *)&i_value;
      break;
    default:
      Tcl_WrongNumArgs(interp, 4, objv, "SUB-COMMAND");
      return TCL_ERROR;
  }

  rc = dbsql_file_control(db, Tcl_GetString(objv[2]), subs[idx].op, arg);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
  return (rc==DBSQL_OK) ? TCL_OK : TCL_ERROR;
}

/*
** This routine registers the custom TCL commands defined in this
** module.  This should be the only procedure visible from outside
** of this module.
*/
int Dbsqlmultiplex_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } cmds[] = {
    { "dbsql_multiplex_initialize", test_multiplex_initialize },
    { "dbsql_multiplex_shutdown", test_multiplex_shutdown },
    { "dbsql_multiplex_control", test_multiplex_control },
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateObjCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }

  return TCL_OK;
}
#endif
