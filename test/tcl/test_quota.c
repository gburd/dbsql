/*
** 2010 September 31
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
** pager and the real VFS.
**
** This particular shim enforces a quota system on files.  One or more
** database files are in a "quota group" that is defined by a GLOB
** pattern.  A quota is set for the combined size of all files in the
** the group.  A quota of zero means "no limit".  If the total size
** of all files in the quota group is greater than the limit, then
** write requests that attempt to enlarge a file fail with DBSQL_FULL.
**
** However, before returning DBSQL_FULL, the write requests invoke
** a callback function that is configurable for each quota group.
** This callback has the opportunity to enlarge the quota.  If the
** callback does enlarge the quota such that the total size of all
** files within the group is less than the new quota, then the write
** continues as if nothing had happened.
*/
#include "test_quota.h"
#include <string.h>
#include <assert.h>

/*
** For an build without mutexes, no-op the mutex calls.
*/
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE==0
#define dbsql_mutex_alloc(X)    ((dbsql_mutex_t*)8)
#define dbsql_mutex_free(X)
#define dbsql_mutex_enter(X)
#define dbsql_mutex_try(X)      DBSQL_OK
#define dbsql_mutex_leave(X)
#define dbsql_mutex_held(X)     ((void)(X),1)
#define dbsql_mutex_notheld(X)  ((void)(X),1)
#endif /* DBSQL_THREADSAFE==0 */


#ifdef _WIN32
# include <windows.h>
# include <io.h>
#else
# include <unistd.h>
#endif


/************************ Object Definitions ******************************/

/* Forward declaration of all object types */
typedef struct quota_group quota_group;
typedef struct quota_conn quota_conn;
typedef struct quota_file quota_file;

/*
** A "quota group" is a collection of files whose collective size we want
** to limit.  Each quota group is defined by a GLOB pattern.
**
** There is an instance of the following object for each defined quota
** group.  This object records the GLOB pattern that defines which files
** belong to the quota group.  The object also remembers the size limit
** for the group (the quota) and the callback to be invoked when the
** sum of the sizes of the files within the group goes over the limit.
**
** A quota group must be established (using dbsql_quota_set(...))
** prior to opening any of the database connections that access files
** within the quota group.
*/
struct quota_group {
  const char *pattern;          /* Filename pattern to be quotaed */
  int64_t i_limit;          /* Upper bound on total file size */
  int64_t i_size;           /* Current size of all files */
  void (*callback)(             /* Callback invoked when going over quota */
     const char *filename,         /* Name of file whose size increases */
     int64_t *pi_limit,        /* IN/OUT: The current limit */
     int64_t i_size,           /* Total size of all files in the group */
     void *arg                     /* Client data */
  );
  void *arg;                    /* Third argument to the callback() */
  void (*destroy)(void*);       /* Optional destructor for arg */
  quota_group *next, **prev;   /* Doubly linked list of all quota objects */
  quota_file *files;             /* Files within this group */
};

/*
** An instance of this structure represents a single file that is part
** of a quota group.  A single file can be opened multiple times.  In
** order keep multiple openings of the same file from causing the size
** of the file to count against the quota multiple times, each file
** has a unique instance of this object and multiple open connections
** to the same file each point to a single instance of this object.
*/
struct quota_file {
  char *filename;                /* Name of this file */
  quota_group *group;             /* Quota group to which this file belongs */
  int64_t i_size;            /* Current size of this file */
  int n_ref;                       /* Number of times this file is open */
  int delete_on_close;              /* True to delete this file when it closes */
  quota_file *next, **prev;     /* Linked list of files in the same group */
};

/*
** An instance of the following object represents each open connection
** to a file that participates in quota tracking.  This object is a
** subclass of dbsql_file_t.  The dbsql_file_t object for the underlying
** VFS is appended to this structure.
*/
struct quota_conn {
  dbsql_file_t base;              /* Base class - must be first */
  quota_file *file;               /* The underlying file */
  /* The underlying VFS dbsql_file_t is appended to this object */
};

/*
** An instance of the following object records the state of an
** open file.  This object is opaque to all users - the internal
** structure is only visible to the functions below.
*/
struct quota_FILE {
  FILE *f;                /* Open stdio file pointer */
  int64_t i_ofst;    /* Current offset into the file */
  quota_file *file;       /* The file record in the quota system */
#ifdef _WIN32
  char *mbcs_name;        /* Full MBCS pathname of the file */
#endif
};


/************************* Global Variables **********************************/
/*
** All global variables used by this file are containing within the following
** __g_quota structure.
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

  /* True when this shim as been initialized.
  */
  int is_initialized;

  /* For run-time access any of the other global data structures in this
  ** shim, the following mutex must be held.
  */
  dbsql_mutex_t *mutex;

  /* List of quota_group objects.
  */
  quota_group *group;

} __g_quota;

/************************* Utility Routines *********************************/
/*
** Acquire and release the mutex used to serialize access to the
** list of quota_groups.
*/
static void __quota_enter(void){ dbsql_mutex_enter(__g_quota.mutex); }
static void __quota_leave(void){ dbsql_mutex_leave(__g_quota.mutex); }

/* Count the number of open files in a quota_group
*/
static int __quota_group_open_file_count(quota_group *group){
  int N = 0;
  quota_file *file = group->files;
  while( file ){
    if( file->n_ref ) N++;
    file = file->next;
  }
  return N;
}

/* Remove a file from a quota group.
*/
static void __quota_remove_file(quota_file *file){
  quota_group *group = file->group;
  group->i_size -= file->i_size;
  *file->prev = file->next;
  if( file->next ) file->next->prev = file->prev;
  dbsql_free(file);
}

/* Remove all files from a quota group.  It is always the case that
** all files will be closed when this routine is called.
*/
static void __quota_remove_all_files(quota_group *group){
  while( group->files ){
    assert( group->files->n_ref==0 );
    __quota_remove_file(group->files);
  }
}


/* If the reference count and threshold for a quota_group are both
** zero, then destroy the quota_group.
*/
static void __quota_group_deref(quota_group *group){
  if( group->i_limit==0 && __quota_group_open_file_count(group)==0 ){
    __quota_remove_all_files(group);
    *group->prev = group->next;
    if( group->next ) group->next->prev = group->prev;
    if( group->destroy ) group->destroy(group->arg);
    dbsql_free(group);
  }
}

/*
** Return TRUE if string z matches glob pattern glob.
**
** Globbing rules:
**
**      '*'       Matches any sequence of zero or more characters.
**
**      '?'       Matches exactly one character.
**
**     [...]      Matches one character from the enclosed list of
**                characters.
**
**     [^...]     Matches one character not in the enclosed list.
**
**     /          Matches "/" or "\\"
**
*/
static int __quota_strglob(const char *glob, const char *z){
  int c, c2, cx;
  int invert;
  int seen;

  while( (c = (*(glob++)))!=0 ){
    if( c=='*' ){
      while( (c=(*(glob++))) == '*' || c=='?' ){
        if( c=='?' && (*(z++))==0 ) return 0;
      }
      if( c==0 ){
        return 1;
      }else if( c=='[' ){
        while( *z && __quota_strglob(glob-1,z)==0 ){
          z++;
        }
        return (*z)!=0;
      }
      cx = (c=='/') ? '\\' : c;
      while( (c2 = (*(z++)))!=0 ){
        while( c2!=c && c2!=cx ){
          c2 = *(z++);
          if( c2==0 ) return 0;
        }
        if( __quota_strglob(glob,z) ) return 1;
      }
      return 0;
    }else if( c=='?' ){
      if( (*(z++))==0 ) return 0;
    }else if( c=='[' ){
      int prior_c = 0;
      seen = 0;
      invert = 0;
      c = *(z++);
      if( c==0 ) return 0;
      c2 = *(glob++);
      if( c2=='^' ){
        invert = 1;
        c2 = *(glob++);
      }
      if( c2==']' ){
        if( c==']' ) seen = 1;
        c2 = *(glob++);
      }
      while( c2 && c2!=']' ){
        if( c2=='-' && glob[0]!=']' && glob[0]!=0 && prior_c>0 ){
          c2 = *(glob++);
          if( c>=prior_c && c<=c2 ) seen = 1;
          prior_c = 0;
        }else{
          if( c==c2 ){
            seen = 1;
          }
          prior_c = c2;
        }
        c2 = *(glob++);
      }
      if( c2==0 || (seen ^ invert)==0 ) return 0;
    }else if( c=='/' ){
      if( z[0]!='/' && z[0]!='\\' ) return 0;
      z++;
    }else{
      if( c!=(*(z++)) ) return 0;
    }
  }
  return *z==0;
}


/* Find a quota_group given the filename.
**
** Return a pointer to the quota_group object. Return NULL if not found.
*/
static quota_group *__quota_group_find(const char *filename){
  quota_group *p;
  for(p=__g_quota.group; p && __quota_strglob(p->pattern, filename)==0;
      p=p->next){}
  return p;
}

/* Translate an dbsql_file_t* that is really a quota_conn* into
** the dbsql_file_t* for the underlying original VFS.
*/
static dbsql_file_t *__quota_sub_open(dbsql_file_t *conn){
  quota_conn *p = (quota_conn*)conn;
  return (dbsql_file_t*)&p[1];
}

/* Find a file in a quota group and return a pointer to that file.
** Return NULL if the file is not in the group.
*/
static quota_file *__quota_find_file(
  quota_group *group,     /* Group in which to look for the file */
  const char *name,      /* Full pathname of the file */
  int create_flag          /* Try to create the file if not found */
){
  quota_file *file = group->files;
  while( file && strcmp(file->filename, name)!=0 ){
    file = file->next;
  }
  if( file==0 && create_flag ){
    int n_name = (int)(strlen(name) & 0x3fffffff);
    file = (quota_file *)dbsql_malloc( sizeof(*file) + n_name + 1 );
    if( file ){
      memset(file, 0, sizeof(*file));
      file->filename = (char*)&file[1];
      memcpy(file->filename, name, n_name+1);
      file->next = group->files;
      if( group->files ) group->files->prev = &file->next;
      file->prev = &group->files;
      group->files = file;
      file->group = group;
    }
  }
  return file;
}
/*
** Translate UTF8 to MBCS for use in fopen() calls.  Return a pointer to the
** translated text..  Call quota_mbcs_free() to deallocate any memory
** used to store the returned pointer when done.
*/
static char *quota_utf8_to_mbcs(const char *utf8){
#ifdef _WIN32
  size_t n;          /* Bytes in utf8 */
  int n_wide;         /* number of UTF-16 characters */
  int n_mbcs;         /* Bytes of MBCS */
  LPWSTR tmp_wide;   /* The UTF16 text */
  char *mbcs;       /* The MBCS text */
  int codepage;      /* Code page used by fopen() */

  n = strlen(utf8);
  n_wide = __multi_byte_to_wide_char(CP_UTF8, 0, utf8, -1, NULL, 0);
  if( n_wide==0 ) return 0;
  tmp_wide = (LPWSTR)dbsql_malloc( (n_wide+1)*sizeof(tmp_wide[0]) );
  if( tmp_wide==0 ) return 0;
  __multi_byte_to_wide_char(CP_UTF8, 0, utf8, -1, tmp_wide, n_wide);
  codepage = __are_file_apis_ansi() ? CP_ACP : CP_OEMCP;
  n_mbcs = __wide_char_to_multi_byte(codepage, 0, tmp_wide, n_wide, 0, 0, 0, 0);
  mbcs = n_mbcs ? (char*)dbsql_malloc( n_mbcs+1 ) : 0;
  if( mbcs ){
    __wide_char_to_multi_byte(codepage, 0, tmp_wide, n_wide, mbcs, n_mbcs, 0, 0);
  }
  dbsql_free(tmp_wide);
  return mbcs;
#else
  return (char*)utf8;  /* No-op on unix */
#endif
}

/*
** Deallocate any memory allocated by quota_utf8_to_mbcs().
*/
static void quota_mbcs_free(char *old){
#ifdef _WIN32
  dbsql_free(old);
#else
  /* No-op on unix */
#endif
}

/************************* VFS Method Wrappers *****************************/
/*
** This is the x_open method used for the "quota" VFS.
**
** Most of the work is done by the underlying original VFS.  This method
** simply links the new file into the appropriate quota group if it is a
** file that needs to be tracked.
*/
static int __quota_open(
  dbsql_vfs_t *vfs,          /* The quota VFS */
  const char *name,          /* Name of file to be opened */
  dbsql_file_t *conn,        /* Fill in this file descriptor */
  int flags,                  /* Flags to control the opening */
  int *out_flags              /* Flags showing results of opening */
){
  int rc;                                    /* Result code */
  quota_conn *quota_open;                     /* The new quota file descriptor */
  quota_file *file;                          /* Corresponding quota_file obj */
  quota_group *group;                        /* The group file belongs to */
  dbsql_file_t *sub_open;                    /* Real file descriptor */
  dbsql_vfs_t *orig_vfs = __g_quota.orig_vfs;   /* Real VFS */

  /* If the file is not a main database file or a WAL, then use the
  ** normal x_open method.
  */
  if( (flags & (DBSQL_OPEN_MAIN_DB|DBSQL_OPEN_WAL))==0 ){
    return orig_vfs->x_open(orig_vfs, name, conn, flags, out_flags);
  }

  /* If the name of the file does not match any quota group, then
  ** use the normal x_open method.
  */
  __quota_enter();
  group = __quota_group_find(name);
  if( group==0 ){
    rc = orig_vfs->x_open(orig_vfs, name, conn, flags, out_flags);
  }else{
    /* If we get to this point, it means the file needs to be quota tracked.
    */
    quota_open = (quota_conn*)conn;
    sub_open = __quota_sub_open(conn);
    rc = orig_vfs->x_open(orig_vfs, name, sub_open, flags, out_flags);
    if( rc==DBSQL_OK ){
      file = __quota_find_file(group, name, 1);
      if( file==0 ){
        __quota_leave();
        sub_open->methods->x_close(sub_open);
        return DBSQL_NOMEM;
      }
      file->delete_on_close = (flags & DBSQL_OPEN_DELETEONCLOSE)!=0;
      file->n_ref++;
      quota_open->file = file;
      if( sub_open->methods->i_version==1 ){
        quota_open->base.methods = &__g_quota.s_io_methods_v1;
      }else{
        quota_open->base.methods = &__g_quota.s_io_methods_v2;
      }
    }
  }
  __quota_leave();
  return rc;
}

/*
** This is the x_delete method used for the "quota" VFS.
**
** If the file being deleted is part of the quota group, then reduce
** the size of the quota group accordingly.  And remove the file from
** the set of files in the quota group.
*/
static int __quota_delete(
  dbsql_vfs_t *vfs,          /* The quota VFS */
  const char *name,          /* Name of file to be deleted */
  int sync_dir                 /* Do a directory sync after deleting */
){
  int rc;                                    /* Result code */
  quota_file *file;                          /* Files in the quota */
  quota_group *group;                        /* The group file belongs to */
  dbsql_vfs_t *orig_vfs = __g_quota.orig_vfs;   /* Real VFS */

  /* Do the actual file delete */
  rc = orig_vfs->x_delete(orig_vfs, name, sync_dir);

  /* If the file just deleted is a member of a quota group, then remove
  ** it from that quota group.
  */
  if( rc==DBSQL_OK ){
    __quota_enter();
    group = __quota_group_find(name);
    if( group ){
      file = __quota_find_file(group, name, 0);
      if( file ){
        if( file->n_ref ){
          file->delete_on_close = 1;
        }else{
          __quota_remove_file(file);
          __quota_group_deref(group);
        }
      }
    }
    __quota_leave();
  }
  return rc;
}


/************************ I/O Method Wrappers *******************************/

/* x_close requests get passed through to the original VFS.  But we
** also have to unlink the quota_conn from the quota_file and quota_group.
** The quota_file and/or quota_group are freed if they are no longer in use.
*/
static int __quota_close(dbsql_file_t *conn){
  quota_conn *p = (quota_conn*)conn;
  quota_file *file = p->file;
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  int rc;
  rc = sub_open->methods->x_close(sub_open);
  __quota_enter();
  file->n_ref--;
  if( file->n_ref==0 ){
    quota_group *group = file->group;
    if( file->delete_on_close ){
      __g_quota.orig_vfs->x_delete(__g_quota.orig_vfs, file->filename, 0);
      __quota_remove_file(file);
    }
    __quota_group_deref(group);
  }
  __quota_leave();
  return rc;
}

/* Pass x_read requests directory thru to the original VFS without
** further processing.
*/
static int __quota_read(
  dbsql_file_t *conn,
  void *buf,
  int i_amt,
  int64_t i_ofst
){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->x_read(sub_open, buf, i_amt, i_ofst);
}

/* Check x_write requests to see if they expand the file.  If they do,
** the perform a quota check before passing them through to the
** original VFS.
*/
static int __quota_write(
  dbsql_file_t *conn,
  const void *buf,
  int i_amt,
  int64_t i_ofst
){
  quota_conn *p = (quota_conn*)conn;
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  int64_t i_end = i_ofst+i_amt;
  quota_group *group;
  quota_file *file = p->file;
  int64_t sz_new;

  if( file->i_size<i_end ){
    group = file->group;
    __quota_enter();
    sz_new = group->i_size - file->i_size + i_end;
    if( sz_new>group->i_limit && group->i_limit>0 ){
      if( group->callback ){
        group->callback(file->filename, &group->i_limit, sz_new,
                          group->arg);
      }
      if( sz_new>group->i_limit && group->i_limit>0 ){
        __quota_leave();
        return DBSQL_FULL;
      }
    }
    group->i_size = sz_new;
    file->i_size = i_end;
    __quota_leave();
  }
  return sub_open->methods->x_write(sub_open, buf, i_amt, i_ofst);
}

/* Pass truncate requests thru to the original VFS.  If the
** success, update the file size.
*/
static int __quota_truncate(dbsql_file_t *conn, int64_t size){
  quota_conn *p = (quota_conn*)conn;
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  int rc = sub_open->methods->truncate(sub_open, size);
  quota_file *file = p->file;
  quota_group *group;
  if( rc==DBSQL_OK ){
    __quota_enter();
    group = file->group;
    group->i_size -= file->i_size;
    file->i_size = size;
    group->i_size += size;
    __quota_leave();
  }
  return rc;
}

/* Pass x_sync requests through to the original VFS without change
*/
static int __quota_sync(dbsql_file_t *conn, int flags){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->x_sync(sub_open, flags);
}

/* Pass file_size requests through to the original VFS but then
** update the quota_group with the new size before returning.
*/
static int __quota_file_size(dbsql_file_t *conn, int64_t *size){
  quota_conn *p = (quota_conn*)conn;
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  quota_file *file = p->file;
  quota_group *group;
  int64_t sz;
  int rc;

  rc = sub_open->methods->file_size(sub_open, &sz);
  if( rc==DBSQL_OK ){
    __quota_enter();
    group = file->group;
    group->i_size -= file->i_size;
    file->i_size = sz;
    group->i_size += sz;
    __quota_leave();
    *size = sz;
  }
  return rc;
}

/* Pass lock requests through to the original VFS unchanged.
*/
static int __quota_lock(dbsql_file_t *conn, int lock){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->lock(sub_open, lock);
}

/* Pass unlock requests through to the original VFS unchanged.
*/
static int __quota_unlock(dbsql_file_t *conn, int lock){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->unlock(sub_open, lock);
}

/* Pass check_reserved_lock requests through to the original VFS unchanged.
*/
static int __quota_check_reserved_lock(dbsql_file_t *conn, int *res_out){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->check_reserved_lock(sub_open, res_out);
}

/* Pass file_control requests through to the original VFS unchanged.
*/
static int __quota_file_control(dbsql_file_t *conn, int op, void *arg){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  int rc = sub_open->methods->file_control(sub_open, op, arg);
#if defined(DBSQL_FCNTL_VFSNAME)
  if( op==DBSQL_FCNTL_VFSNAME && rc==DBSQL_OK ){
    *(char**)arg = dbsql_mprintf("quota/%z", *(char**)arg);
  }
#endif
  return rc;
}

/* Pass sector_size requests through to the original VFS unchanged.
*/
static int __quota_sector_size(dbsql_file_t *conn){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->sector_size(sub_open);
}

/* Pass device_characteristics requests through to the original VFS unchanged.
*/
static int __quota_device_characteristics(dbsql_file_t *conn){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->device_characteristics(sub_open);
}

/* Pass shm_map requests through to the original VFS unchanged.
*/
static int __quota_shm_map(
  dbsql_file_t *conn,            /* Handle open on database file */
  int i_region,                    /* Region to retrieve */
  int sz_region,                   /* Size of regions */
  int extend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->shm_map(sub_open, i_region, sz_region, extend, pp);
}

/* Pass shm_lock requests through to the original VFS unchanged.
*/
static int __quota_shm_lock(
  dbsql_file_t *conn,       /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->shm_lock(sub_open, ofst, n, flags);
}

/* Pass shm_barrier requests through to the original VFS unchanged.
*/
static void __quota_shm_barrier(dbsql_file_t *conn){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  sub_open->methods->shm_barrier(sub_open);
}

/* Pass shm_unmap requests through to the original VFS unchanged.
*/
static int __quota_shm_unmap(dbsql_file_t *conn, int delete_flag){
  dbsql_file_t *sub_open = __quota_sub_open(conn);
  return sub_open->methods->shm_unmap(sub_open, delete_flag);
}

/************************** Public Interfaces *****************************/
/*
** Initialize the quota VFS shim.  Use the VFS named orig_vfs_name
** as the VFS that does the actual work.  Use the default if
** orig_vfs_name==NULL.
**
** The quota VFS shim is named "quota".  It will become the default
** VFS if make_default is non-zero.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once
** during start-up.
*/
int dbsql_quota_initialize(const char *orig_vfs_name, int make_default){
  dbsql_vfs_t *orig_vfs;
  if( __g_quota.is_initialized ) return DBSQL_MISUSE;
  orig_vfs = dbsql_vfs_find(orig_vfs_name);
  if( orig_vfs==0 ) return DBSQL_ERROR;
  assert( orig_vfs!=&__g_quota.s_this_vfs );
  __g_quota.mutex = dbsql_mutex_alloc(DBSQL_MUTEX_FAST);
  if( !__g_quota.mutex ){
    return DBSQL_NOMEM;
  }
  __g_quota.is_initialized = 1;
  __g_quota.orig_vfs = orig_vfs;
  __g_quota.s_this_vfs = *orig_vfs;
  __g_quota.s_this_vfs.x_open = __quota_open;
  __g_quota.s_this_vfs.x_delete = __quota_delete;
  __g_quota.s_this_vfs.sz_os_file += sizeof(quota_conn);
  __g_quota.s_this_vfs.name = "quota";
  __g_quota.s_io_methods_v1.i_version = 1;
  __g_quota.s_io_methods_v1.x_close = __quota_close;
  __g_quota.s_io_methods_v1.x_read = __quota_read;
  __g_quota.s_io_methods_v1.x_write = __quota_write;
  __g_quota.s_io_methods_v1.truncate = __quota_truncate;
  __g_quota.s_io_methods_v1.x_sync = __quota_sync;
  __g_quota.s_io_methods_v1.file_size = __quota_file_size;
  __g_quota.s_io_methods_v1.lock = __quota_lock;
  __g_quota.s_io_methods_v1.unlock = __quota_unlock;
  __g_quota.s_io_methods_v1.check_reserved_lock = __quota_check_reserved_lock;
  __g_quota.s_io_methods_v1.file_control = __quota_file_control;
  __g_quota.s_io_methods_v1.sector_size = __quota_sector_size;
  __g_quota.s_io_methods_v1.device_characteristics = __quota_device_characteristics;
  __g_quota.s_io_methods_v2 = __g_quota.s_io_methods_v1;
  __g_quota.s_io_methods_v2.i_version = 2;
  __g_quota.s_io_methods_v2.shm_map = __quota_shm_map;
  __g_quota.s_io_methods_v2.shm_lock = __quota_shm_lock;
  __g_quota.s_io_methods_v2.shm_barrier = __quota_shm_barrier;
  __g_quota.s_io_methods_v2.shm_unmap = __quota_shm_unmap;
  dbsql_vfs_register(&__g_quota.s_this_vfs, make_default);
  return DBSQL_OK;
}

/*
** Shutdown the quota system.
**
** All dbsql database connections must be closed before calling this
** routine.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once while
** shutting down in order to free all remaining quota groups.
*/
int dbsql_quota_shutdown(void){
  quota_group *group;
  if( __g_quota.is_initialized==0 ) return DBSQL_MISUSE;
  for(group=__g_quota.group; group; group=group->next){
    if( __quota_group_open_file_count(group)>0 ) return DBSQL_MISUSE;
  }
  while( __g_quota.group ){
    group = __g_quota.group;
    __g_quota.group = group->next;
    group->i_limit = 0;
    assert( __quota_group_open_file_count(group)==0 );
    __quota_group_deref(group);
  }
  __g_quota.is_initialized = 0;
  dbsql_mutex_free(__g_quota.mutex);
  dbsql_vfs_unregister(&__g_quota.s_this_vfs);
  memset(&__g_quota, 0, sizeof(__g_quota));
  return DBSQL_OK;
}

/*
** Create or destroy a quota group.
**
** The quota group is defined by the pattern.  When calling this routine
** with a pattern for a quota group that already exists, this routine
** merely updates the i_limit, callback, and arg values for that quota
** group.  If pattern is new, then a new quota group is created.
**
** If the i_limit for a quota group is set to zero, then the quota group
** is disabled and will be deleted when the last database connection using
** the quota group is closed.
**
** Calling this routine on a pattern that does not exist and with a
** zero i_limit is a no-op.
**
** A quota group must exist with a non-zero i_limit prior to opening
** database connections if those connections are to participate in the
** quota group.  Creating a quota group does not affect database connections
** that are already open.
*/
int dbsql_quota_set(
  const char *pattern,           /* The filename pattern */
  int64_t i_limit,           /* New quota to set for this quota group */
  void (*callback)(              /* Callback invoked when going over quota */
     const char *filename,         /* Name of file whose size increases */
     int64_t *pi_limit,        /* IN/OUT: The current limit */
     int64_t i_size,           /* Total size of all files in the group */
     void *arg                     /* Client data */
  ),
  void *arg,                     /* client data passed thru to callback */
  void (*destroy)(void*)         /* Optional destructor for arg */
){
  quota_group *group;
  __quota_enter();
  group = __g_quota.group;
  while( group && strcmp(group->pattern, pattern)!=0 ){
    group = group->next;
  }
  if( group==0 ){
    int n_pattern = (int)(strlen(pattern) & 0x3fffffff);
    if( i_limit<=0 ){
      __quota_leave();
      return DBSQL_OK;
    }
    group = (quota_group *)dbsql_malloc( sizeof(*group) + n_pattern + 1 );
    if( group==0 ){
      __quota_leave();
      return DBSQL_NOMEM;
    }
    memset(group, 0, sizeof(*group));
    group->pattern = (char*)&group[1];
    memcpy((char *)group->pattern, pattern, n_pattern+1);
    if( __g_quota.group ) __g_quota.group->prev = &group->next;
    group->next = __g_quota.group;
    group->prev = &__g_quota.group;
    __g_quota.group = group;
  }
  group->i_limit = i_limit;
  group->callback = callback;
  if( group->destroy && group->arg!=arg ){
    group->destroy(group->arg);
  }
  group->arg = arg;
  group->destroy = destroy;
  __quota_group_deref(group);
  __quota_leave();
  return DBSQL_OK;
}

/*
** Bring the named file under quota management.  Or if it is already under
** management, update its size.
*/
int dbsql_quota_file(const char *filename){
  char *full = 0;
  dbsql_file_t *fd;
  int rc;
  int out_flags = 0;
  int64_t i_size;
  int n_alloc = __g_quota.s_this_vfs.sz_os_file + __g_quota.s_this_vfs.mx_pathname+2;

  /* Allocate space for a file-handle and the full path for file filename */
  fd = (dbsql_file_t *)dbsql_malloc(n_alloc);
  if( fd==0 ){
    rc = DBSQL_NOMEM;
  }else{
    full = &((char *)fd)[__g_quota.s_this_vfs.sz_os_file];
    rc = __g_quota.orig_vfs->full_pathname(__g_quota.orig_vfs, filename,
        __g_quota.s_this_vfs.mx_pathname+1, full);
  }

  if( rc==DBSQL_OK ){
    full[strlen(full)+1] = '\0';
    rc = __quota_open(&__g_quota.s_this_vfs, full, fd,
                   DBSQL_OPEN_READONLY | DBSQL_OPEN_MAIN_DB, &out_flags);
    if( rc==DBSQL_OK ){
      fd->methods->file_size(fd, &i_size);
      fd->methods->x_close(fd);
    }else if( rc==DBSQL_CANTOPEN ){
      quota_group *group;
      quota_file *file;
      __quota_enter();
      group = __quota_group_find(full);
      if( group ){
        file = __quota_find_file(group, full, 0);
        if( file ) __quota_remove_file(file);
      }
      __quota_leave();
    }
  }

  dbsql_free(fd);
  return rc;
}

/*
** Open a potentially quotaed file for I/O.
*/
quota_FILE *dbsql_quota_fopen(const char *filename, const char *mode){
  quota_FILE *p = 0;
  char *full = 0;
  char *full_translated = 0;
  int rc;
  quota_group *group;
  quota_file *file;

  full = (char*)dbsql_malloc(__g_quota.s_this_vfs.mx_pathname + 1);
  if( full==0 ) return 0;
  rc = __g_quota.orig_vfs->full_pathname(__g_quota.orig_vfs, filename,
                                      __g_quota.s_this_vfs.mx_pathname+1, full);
  if( rc ) goto quota_fopen_error;
  p = (quota_FILE*)dbsql_malloc(sizeof(*p));
  if( p==0 ) goto quota_fopen_error;
  memset(p, 0, sizeof(*p));
  full_translated = quota_utf8_to_mbcs(full);
  if( full_translated==0 ) goto quota_fopen_error;
  p->f = fopen(full_translated, mode);
  if( p->f==0 ) goto quota_fopen_error;
  __quota_enter();
  group = __quota_group_find(full);
  if( group ){
    file = __quota_find_file(group, full, 1);
    if( file==0 ){
      __quota_leave();
      goto quota_fopen_error;
    }
    file->n_ref++;
    p->file = file;
  }
  __quota_leave();
  dbsql_free(full);
#ifdef _WIN32
  p->mbcs_name = full_translated;
#endif
  return p;

quota_fopen_error:
  quota_mbcs_free(full_translated);
  dbsql_free(full);
  if( p && p->f ) fclose(p->f);
  dbsql_free(p);
  return 0;
}

/*
** Read content from a quota_FILE
*/
size_t dbsql_quota_fread(
  void *buf,            /* Store the content here */
  size_t size,           /* Size of each element */
  size_t nmemb,          /* Number of elements to read */
  quota_FILE *p          /* Read from this quota_FILE object */
){
  return fread(buf, size, nmemb, p->f);
}

/*
** Write content into a quota_FILE.  Invoke the quota callback and block
** the write if we exceed quota.
*/
size_t dbsql_quota_fwrite(
  const void *buf,      /* Take content to write from here */
  size_t size,           /* Size of each element */
  size_t nmemb,          /* Number of elements */
  quota_FILE *p          /* Write to this quota_FILE object */
){
  int64_t i_ofst;
  int64_t i_end;
  int64_t sz_new;
  quota_file *file;
  size_t rc;

  i_ofst = ftell(p->f);
  i_end = i_ofst + size*nmemb;
  file = p->file;
  if( file && file->i_size<i_end ){
    quota_group *group = file->group;
    __quota_enter();
    sz_new = group->i_size - file->i_size + i_end;
    if( sz_new>group->i_limit && group->i_limit>0 ){
      if( group->callback ){
        group->callback(file->filename, &group->i_limit, sz_new,
                          group->arg);
      }
      if( sz_new>group->i_limit && group->i_limit>0 ){
        i_end = group->i_limit - group->i_size + file->i_size;
        nmemb = (size_t)((i_end - i_ofst)/size);
        i_end = i_ofst + size*nmemb;
        sz_new = group->i_size - file->i_size + i_end;
      }
    }
    group->i_size = sz_new;
    file->i_size = i_end;
    __quota_leave();
  }else{
    file = 0;
  }
  rc = fwrite(buf, size, nmemb, p->f);

  /* If the write was incomplete, adjust the file size and group size
  ** downward */
  if( rc<nmemb && file ){
    size_t n_written = rc;
    int64_t i_new_end = i_ofst + size*n_written;
    if( i_new_end<i_end ) i_new_end = i_end;
    __quota_enter();
    file->group->i_size += i_new_end - file->i_size;
    file->i_size = i_new_end;
    __quota_leave();
  }
  return rc;
}

/*
** Close an open quota_FILE stream.
*/
int dbsql_quota_fclose(quota_FILE *p){
  int rc;
  quota_file *file;
  rc = fclose(p->f);
  file = p->file;
  if( file ){
    __quota_enter();
    file->n_ref--;
    if( file->n_ref==0 ){
      quota_group *group = file->group;
      if( file->delete_on_close ){
        __g_quota.orig_vfs->x_delete(__g_quota.orig_vfs, file->filename, 0);
        __quota_remove_file(file);
      }
      __quota_group_deref(group);
    }
    __quota_leave();
  }
#ifdef _WIN32
  quota_mbcs_free(p->mbcs_name);
#endif
  dbsql_free(p);
  return rc;
}

/*
** Flush memory buffers for a quota_FILE to disk.
*/
int dbsql_quota_fflush(quota_FILE *p, int do_fsync){
  int rc;
  rc = fflush(p->f);
  if( rc==0 && do_fsync ){
#ifdef _WIN32
    rc = _commit(_fileno(p->f));
#else
    rc = fsync(fileno(p->f));
#endif
  }
  return rc!=0;
}

/*
** Seek on a quota_FILE stream.
*/
int dbsql_quota_fseek(quota_FILE *p, long offset, int whence){
  return fseek(p->f, offset, whence);
}

/*
** rewind a quota_FILE stream.
*/
void dbsql_quota_rewind(quota_FILE *p){
  rewind(p->f);
}

/*
** Tell the current location of a quota_FILE stream.
*/
long dbsql_quota_ftell(quota_FILE *p){
  return ftell(p->f);
}

/*
** Test the error indicator for the given file.
*/
int dbsql_quota_ferror(quota_FILE *p){
  return ferror(p->f);
}

/*
** Truncate a file to sz_new bytes.
*/
int dbsql_quota_ftruncate(quota_FILE *p, int64_t sz_new){
  quota_file *file = p->file;
  int rc;
  if( (file = p->file)!=0 && file->i_size<sz_new ){
    quota_group *group;
    if( file->i_size<sz_new ){
      /* This routine cannot be used to extend a file that is under
      ** quota management.  Only true truncation is allowed. */
      return -1;
    }
    group = file->group;
    __quota_enter();
    group->i_size += sz_new - file->i_size;
    __quota_leave();
  }
#ifdef _WIN32
#  if defined(__MSVCRT__) && defined(DBSQL_TEST)
     /* _chsize_s() is missing from __ming_w (as of 2012-11-06).  Use
     ** _chsize() as a work-around for testing purposes. */
     rc = _chsize(_fileno(p->f), (long)sz_new);
#  else
     rc = _chsize_s(_fileno(p->f), sz_new);
#  endif
#else
  rc = ftruncate(fileno(p->f), sz_new);
#endif
  if( file && rc==0 ){
    quota_group *group = file->group;
    __quota_enter();
    group->i_size += sz_new - file->i_size;
    file->i_size = sz_new;
    __quota_leave();
  }
  return rc;
}

/*
** Determine the time that the given file was last modified, in
** seconds size 1970.  Write the result into *p_time.  Return 0 on
** success and non-zero on any kind of error.
*/
int dbsql_quota_file_mtime(quota_FILE *p, time_t *p_time){
  int rc;
#ifdef _WIN32
  struct _stati64 buf;
  rc = _stati64(p->mbcs_name, &buf);
#else
  struct stat buf;
  rc = fstat(fileno(p->f), &buf);
#endif
  if( rc==0 ) *p_time = buf.st_mtime;
  return rc;
}

/*
** Return the true size of the file, as reported by the operating
** system.
*/
int64_t dbsql_quota_file_truesize(quota_FILE *p){
  int rc;
#ifdef _WIN32
  struct _stati64 buf;
  rc = _stati64(p->mbcs_name, &buf);
#else
  struct stat buf;
  rc = fstat(fileno(p->f), &buf);
#endif
  return rc==0 ? buf.st_size : -1;
}

/*
** Return the size of the file, as it is known to the quota subsystem.
*/
int64_t dbsql_quota_file_size(quota_FILE *p){
  return p->file ? p->file->i_size : -1;
}

/*
** Determine the amount of data in bytes available for reading
** in the given file.
*/
long dbsql_quota_file_available(quota_FILE *p){
  FILE* f = p->f;
  long pos1, pos2;
  int rc;
  pos1 = ftell(f);
  if ( pos1 < 0 ) return -1;
  rc = fseek(f, 0, SEEK_END);
  if ( rc != 0 ) return -1;
  pos2 = ftell(f);
  if ( pos2 < 0 ) return -1;
  rc = fseek(f, pos1, SEEK_SET);
  if ( rc != 0 ) return -1;
  return pos2 - pos1;
}

/*
** Remove a managed file.  Update quotas accordingly.
*/
int dbsql_quota_remove(const char *filename){
  char *full;            /* Full pathname for filename */
  size_t n_full;           /* Number of bytes in filename */
  int rc;                 /* Result code */
  quota_group *group;     /* Group containing filename */
  quota_file *file;       /* A file in the group */
  quota_file *next_file;   /* next file in the group */
  int diff;               /* Difference between filenames */
  char c;                 /* First character past end of pattern */

  full = (char*)dbsql_malloc(__g_quota.s_this_vfs.mx_pathname + 1);
  if( full==0 ) return DBSQL_NOMEM;
  rc = __g_quota.orig_vfs->full_pathname(__g_quota.orig_vfs, filename,
                                      __g_quota.s_this_vfs.mx_pathname+1, full);
  if( rc ){
    dbsql_free(full);
    return rc;
  }

  /* Figure out the length of the full pathname.  If the name ends with
  ** / (or \ on windows) then remove the trailing /.
  */
  n_full = strlen(full);
  if( n_full>0 && (full[n_full-1]=='/' || full[n_full-1]=='\\') ){
    n_full--;
    full[n_full] = 0;
  }

  __quota_enter();
  group = __quota_group_find(full);
  if( group ){
    for(file=group->files; file && rc==DBSQL_OK; file=next_file){
      next_file = file->next;
      diff = strncmp(full, file->filename, n_full);
      if( diff==0 && ((c = file->filename[n_full])==0 || c=='/' || c=='\\') ){
        if( file->n_ref ){
          file->delete_on_close = 1;
        }else{
          rc = __g_quota.orig_vfs->x_delete(__g_quota.orig_vfs, file->filename, 0);
          __quota_remove_file(file);
          __quota_group_deref(group);
        }
      }
    }
  }
  __quota_leave();
  dbsql_free(full);
  return rc;
}

/***************************** Test Code ***********************************/
#ifdef DBSQL_TEST
#include "tcl_dbsql.h"

/*
** Argument passed to a TCL quota-over-limit callback.
*/
typedef struct tcl_quota_callback_t tcl_quota_callback_t;
struct tcl_quota_callback_t {
  Tcl_Interp *interp;    /* Interpreter in which to run the script */
  Tcl_Obj *script;      /* Script to be run */
};

extern const char *__err_name(int);


/*
** This is the callback from a quota-over-limit.
*/
static void __tcl_quota_callback(
  const char *filename,          /* Name of file whose size increases */
  int64_t *pi_limit,         /* IN/OUT: The current limit */
  int64_t i_size,            /* Total size of all files in the group */
  void *arg                      /* Client data */
){
  tcl_quota_callback_t *p;            /* Callback script object */
  Tcl_Obj *eval;                 /* Script to evaluate */
  Tcl_Obj *varname;              /* Name of variable to pass as 2nd arg */
  unsigned int rnd;               /* Random part of varname */
  int rc;                         /* Tcl error code */

  p = (tcl_quota_callback_t *)arg;
  if( p==0 ) return;

  varname = Tcl_NewStringObj("::piLimit_", -1);
  Tcl_IncrRefCount(varname);
  dbsql_randomness(sizeof(rnd), (void *)&rnd);
  Tcl_AppendObjToObj(varname, Tcl_NewIntObj((int)(rnd&0x7FFFFFFF)));
  Tcl_ObjSetVar2(p->interp, varname, 0, Tcl_NewWideIntObj(*pi_limit), 0);

  eval = Tcl_DuplicateObj(p->script);
  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(0, eval, Tcl_NewStringObj(filename, -1));
  Tcl_ListObjAppendElement(0, eval, varname);
  Tcl_ListObjAppendElement(0, eval, Tcl_NewWideIntObj(i_size));
  rc = Tcl_EvalObjEx(p->interp, eval, TCL_EVAL_GLOBAL);

  if( rc==TCL_OK ){
    Tcl_WideInt x;
    Tcl_Obj *limit = Tcl_ObjGetVar2(p->interp, varname, 0, 0);
    rc = Tcl_GetWideIntFromObj(p->interp, limit, &x);
    *pi_limit = x;
    Tcl_UnsetVar(p->interp, Tcl_GetString(varname), 0);
  }

  Tcl_DecrRefCount(eval);
  Tcl_DecrRefCount(varname);
  if( rc!=TCL_OK ) Tcl_BackgroundError(p->interp);
}

/*
** Destructor for a TCL quota-over-limit callback.
*/
static void __tcl_callback_destructor(void *obj){
  tcl_quota_callback_t *p = (tcl_quota_callback_t*)obj;
  if( p ){
    Tcl_DecrRefCount(p->script);
    dbsql_free((char *)p);
  }
}

/*
** tclcmd: dbsql_quota_initialize NAME MAKEDEFAULT
*/
static int DBSQL_TCLAPI test_quota_initialize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *name;              /* Name of new quota VFS */
  int make_default;                /* True to make the new VFS the default */
  int rc;                         /* Value returned by quota_initialize() */

  /* Process arguments */
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME MAKEDEFAULT");
    return TCL_ERROR;
  }
  name = Tcl_GetString(objv[1]);
  if( Tcl_GetBooleanFromObj(interp, objv[2], &make_default) ) return TCL_ERROR;
  if( name[0]=='\0' ) name = 0;

  /* Call dbsql_quota_initialize() */
  rc = dbsql_quota_initialize(name, make_default);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_shutdown
*/
static int DBSQL_TCLAPI test_quota_shutdown(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Value returned by quota_shutdown() */

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  /* Call dbsql_quota_shutdown() */
  rc = dbsql_quota_shutdown();
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_set PATTERN LIMIT SCRIPT
*/
static int DBSQL_TCLAPI test_quota_set(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *pattern;           /* File pattern to configure */
  Tcl_WideInt i_limit;             /* Initial quota in bytes */
  Tcl_Obj *script;               /* Tcl script to invoke to increase quota */
  int rc;                         /* Value returned by quota_set() */
  tcl_quota_callback_t *p;            /* Callback object */
  TCL_SIZE n_script;               /* Length of callback script */
  void (*destroy)(void*);        /* Optional destructor for arg */
  void (*callback)(const char *, int64_t *, int64_t, void *);

  /* Process arguments */
  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PATTERN LIMIT SCRIPT");
    return TCL_ERROR;
  }
  pattern = Tcl_GetString(objv[1]);
  if( Tcl_GetWideIntFromObj(interp, objv[2], &i_limit) ) return TCL_ERROR;
  script = objv[3];
  Tcl_GetStringFromObj(script, &n_script);

  if( n_script>0 ){
    /* Allocate a tcl_quota_callback_t object */
    p = (tcl_quota_callback_t *)dbsql_malloc(sizeof(tcl_quota_callback_t));
    if( !p ){
      Tcl_SetResult(interp, (char *)"DBSQL_NOMEM", TCL_STATIC);
      return TCL_OK;
    }
    memset(p, 0, sizeof(tcl_quota_callback_t));
    p->interp = interp;
    Tcl_IncrRefCount(script);
    p->script = script;
    destroy = __tcl_callback_destructor;
    callback = __tcl_quota_callback;
  }else{
    p = 0;
    destroy = 0;
    callback = 0;
  }

  /* Invoke dbsql_quota_set() */
  rc = dbsql_quota_set(pattern, i_limit, callback, (void*)p, destroy);

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_file FILENAME
*/
static int DBSQL_TCLAPI test_quota_file(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *filename;          /* File pattern to configure */
  int rc;                         /* Value returned by quota_file() */

  /* Process arguments */
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }
  filename = Tcl_GetString(objv[1]);

  /* Invoke dbsql_quota_file() */
  rc = dbsql_quota_file(filename);

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
  return TCL_OK;
}

/*
** tclcmd:  dbsql_quota_dump
*/
static int DBSQL_TCLAPI test_quota_dump(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Obj *result;
  Tcl_Obj *group_term;
  Tcl_Obj *file_term;
  quota_group *group;
  quota_file *file;

  result = Tcl_NewObj();
  __quota_enter();
  for(group=__g_quota.group; group; group=group->next){
    group_term = Tcl_NewObj();
    Tcl_ListObjAppendElement(interp, group_term,
          Tcl_NewStringObj(group->pattern, -1));
    Tcl_ListObjAppendElement(interp, group_term,
          Tcl_NewWideIntObj(group->i_limit));
    Tcl_ListObjAppendElement(interp, group_term,
          Tcl_NewWideIntObj(group->i_size));
    for(file=group->files; file; file=file->next){
      int i;
      char temp[1000];
      file_term = Tcl_NewObj();
      dbsql_snprintf(sizeof(temp), temp, "%s", file->filename);
      for(i=0; temp[i]; i++){ if( temp[i]=='\\' ) temp[i] = '/'; }
      Tcl_ListObjAppendElement(interp, file_term,
            Tcl_NewStringObj(temp, -1));
      Tcl_ListObjAppendElement(interp, file_term,
            Tcl_NewWideIntObj(file->i_size));
      Tcl_ListObjAppendElement(interp, file_term,
            Tcl_NewWideIntObj(file->n_ref));
      Tcl_ListObjAppendElement(interp, file_term,
            Tcl_NewWideIntObj(file->delete_on_close));
      Tcl_ListObjAppendElement(interp, group_term, file_term);
    }
    Tcl_ListObjAppendElement(interp, result, group_term);
  }
  __quota_leave();
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_fopen FILENAME MODE
*/
static int DBSQL_TCLAPI test_quota_fopen(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *filename;          /* File pattern to configure */
  const char *mode;              /* shell_mode_t string */
  quota_FILE *p;                  /* Open string object */
  char z_return[50];               /* Name of pointer to return */

  /* Process arguments */
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME MODE");
    return TCL_ERROR;
  }
  filename = Tcl_GetString(objv[1]);
  mode = Tcl_GetString(objv[2]);
  p = dbsql_quota_fopen(filename, mode);
  dbsql_snprintf(sizeof(z_return), z_return, "%p", p);
  Tcl_SetResult(interp, z_return, TCL_VOLATILE);
  return TCL_OK;
}

/* Defined in test1.c */
extern void *__test_text_to_ptr(const char*);

/*
** tclcmd: dbsql_quota_fread HANDLE SIZE NELEM
*/
static int DBSQL_TCLAPI test_quota_fread(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  char *buf;
  int sz;
  int n_elem;
  size_t got;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE SIZE NELEM");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  if( Tcl_GetIntFromObj(interp, objv[2], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[3], &n_elem) ) return TCL_ERROR;
  buf = (char*)dbsql_malloc( sz*n_elem + 1 );
  if( buf==0 ){
    Tcl_SetResult(interp, "out of memory", TCL_STATIC);
    return TCL_ERROR;
  }
  got = dbsql_quota_fread(buf, sz, n_elem, p);
  buf[got*sz] = 0;
  Tcl_SetResult(interp, buf, TCL_VOLATILE);
  dbsql_free(buf);
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_fwrite HANDLE SIZE NELEM CONTENT
*/
static int DBSQL_TCLAPI test_quota_fwrite(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  char *buf;
  int sz;
  int n_elem;
  size_t got;

  if( objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE SIZE NELEM CONTENT");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  if( Tcl_GetIntFromObj(interp, objv[2], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[3], &n_elem) ) return TCL_ERROR;
  buf = Tcl_GetString(objv[4]);
  got = dbsql_quota_fwrite(buf, sz, n_elem, p);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(got));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_fclose HANDLE
*/
static int DBSQL_TCLAPI test_quota_fclose(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  rc = dbsql_quota_fclose(p);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_fflush HANDLE ?HARDSYNC?
*/
static int DBSQL_TCLAPI test_quota_fflush(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int rc;
  int do_sync = 0;

  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE ?HARDSYNC?");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  if( objc==3 ){
    if( Tcl_GetBooleanFromObj(interp, objv[2], &do_sync) ) return TCL_ERROR;
  }
  rc = dbsql_quota_fflush(p, do_sync);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_fseek HANDLE OFFSET WHENCE
*/
static int DBSQL_TCLAPI test_quota_fseek(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int ofst;
  const char *z_whence;
  int whence;
  int rc;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE OFFSET WHENCE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  if( Tcl_GetIntFromObj(interp, objv[2], &ofst) ) return TCL_ERROR;
  z_whence = Tcl_GetString(objv[3]);
  if( strcmp(z_whence, "SEEK_SET")==0 ){
    whence = SEEK_SET;
  }else if( strcmp(z_whence, "SEEK_CUR")==0 ){
    whence = SEEK_CUR;
  }else if( strcmp(z_whence, "SEEK_END")==0 ){
    whence = SEEK_END;
  }else{
    Tcl_AppendResult(interp,
           "WHENCE should be SEEK_SET, SEEK_CUR, or SEEK_END", (char*)0);
    return TCL_ERROR;
  }
  rc = dbsql_quota_fseek(p, ofst, whence);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_rewind HANDLE
*/
static int DBSQL_TCLAPI test_quota_rewind(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  dbsql_quota_rewind(p);
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_ftell HANDLE
*/
static int DBSQL_TCLAPI test_quota_ftell(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int64_t x;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  x = dbsql_quota_ftell(p);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(x));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_ftruncate HANDLE SIZE
*/
static int DBSQL_TCLAPI test_quota_ftruncate(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int64_t x;
  Tcl_WideInt w;
  int rc;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE SIZE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  if( Tcl_GetWideIntFromObj(interp, objv[2], &w) ) return TCL_ERROR;
  x = (int64_t)w;
  rc = dbsql_quota_ftruncate(p, x);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_file_size HANDLE
*/
static int DBSQL_TCLAPI test_quota_file_size(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int64_t x;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  x = dbsql_quota_file_size(p);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(x));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_file_truesize HANDLE
*/
static int DBSQL_TCLAPI test_quota_file_truesize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int64_t x;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  x = dbsql_quota_file_truesize(p);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(x));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_file_mtime HANDLE
*/
static int DBSQL_TCLAPI test_quota_file_mtime(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  time_t t;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  t = 0;
  dbsql_quota_file_mtime(p, &t);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(t));
  return TCL_OK;
}


/*
** tclcmd: dbsql_quota_remove FILENAME
*/
static int DBSQL_TCLAPI test_quota_remove(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *filename;          /* File pattern to configure */
  int rc;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }
  filename = Tcl_GetString(objv[1]);
  rc = dbsql_quota_remove(filename);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_glob PATTERN TEXT
**
** Test the glob pattern matching.  Return 1 if TEXT matches PATTERN
** and return 0 if it does not.
*/
static int DBSQL_TCLAPI test_quota_glob(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *pattern;          /* The glob pattern */
  const char *text;             /* Text to compare against the pattern */
  int rc;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PATTERN TEXT");
    return TCL_ERROR;
  }
  pattern = Tcl_GetString(objv[1]);
  text = Tcl_GetString(objv[2]);
  rc = __quota_strglob(pattern, text);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_file_available HANDLE
**
** Return the number of bytes from the current file point to the end of
** the file.
*/
static int DBSQL_TCLAPI test_quota_file_available(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int64_t x;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  x = dbsql_quota_file_available(p);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(x));
  return TCL_OK;
}

/*
** tclcmd: dbsql_quota_ferror HANDLE
**
** Return true if the file handle is in the error state.
*/
static int DBSQL_TCLAPI test_quota_ferror(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  quota_FILE *p;
  int x;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }
  p = __test_text_to_ptr(Tcl_GetString(objv[1]));
  x = dbsql_quota_ferror(p);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(x));
  return TCL_OK;
}

/*
** This routine registers the custom TCL commands defined in this
** module.  This should be the only procedure visible from outside
** of this module.
*/
int Dbsqlquota_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } cmds[] = {
    { "dbsql_quota_initialize",    test_quota_initialize },
    { "dbsql_quota_shutdown",      test_quota_shutdown },
    { "dbsql_quota_set",           test_quota_set },
    { "dbsql_quota_file",          test_quota_file },
    { "dbsql_quota_dump",          test_quota_dump },
    { "dbsql_quota_fopen",         test_quota_fopen },
    { "dbsql_quota_fread",         test_quota_fread },
    { "dbsql_quota_fwrite",        test_quota_fwrite },
    { "dbsql_quota_fclose",        test_quota_fclose },
    { "dbsql_quota_fflush",        test_quota_fflush },
    { "dbsql_quota_fseek",         test_quota_fseek },
    { "dbsql_quota_rewind",        test_quota_rewind },
    { "dbsql_quota_ftell",         test_quota_ftell },
    { "dbsql_quota_ftruncate",     test_quota_ftruncate },
    { "dbsql_quota_file_size",     test_quota_file_size },
    { "dbsql_quota_file_truesize", test_quota_file_truesize },
    { "dbsql_quota_file_mtime",    test_quota_file_mtime },
    { "dbsql_quota_remove",        test_quota_remove },
    { "dbsql_quota_glob",          test_quota_glob },
    { "dbsql_quota_file_available",test_quota_file_available },
    { "dbsql_quota_ferror",        test_quota_ferror },
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateObjCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }

  return TCL_OK;
}
#endif
