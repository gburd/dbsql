/*
** 2014-06-13
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
** This dbsql extension implements SQL functions readfile() and
** writefile(), and eponymous virtual type "fsdir".
**
** WRITEFILE(FILE, DATA [, MODE [, MTIME]]):
**
**   If neither of the optional arguments is present, then this UDF
**   function writes blob DATA to file FILE. If successful, the number
**   of bytes written is returned. If an error occurs, NULL is returned.
**
**   If the first option argument - MODE - is present, then it must
**   be passed an integer value that corresponds to a POSIX mode
**   value (file type + permissions, as returned in the stat.st_mode
**   field by the stat() system call). Three types of files may
**   be written/created:
**
**     regular files:  (mode & 0170000)==0100000
**     symbolic links: (mode & 0170000)==0120000
**     directories:    (mode & 0170000)==0040000
**
**   For a directory, the DATA is ignored. For a symbolic link, it is
**   interpreted as text and used as the target of the link. For a
**   regular file, it is interpreted as a blob and written into the
**   named file. Regardless of the type of file, its permissions are
**   set to (mode & 0777) before returning.
**
**   If the optional MTIME argument is present, then it is interpreted
**   as an integer - the number of seconds since the unix epoch. The
**   modification-time of the target file is set to this value before
**   returning.
**
**   If five or more arguments are passed to this function and an
**   error is encountered, an exception is raised.
**
** READFILE(FILE):
**
**   Read and return the contents of file FILE (type blob) from disk.
**
** FSDIR:
**
**   Used as follows:
**
**     SELECT * FROM fsdir($path [, $dir]);
**
**   Parameter $path is an absolute or relative pathname. If the file that it
**   refers to does not exist, it is an error. If the path refers to a regular
**   file or symbolic link, it returns a single row. Or, if the path refers
**   to a directory, it returns one row for the directory, and one row for each
**   file within the hierarchy rooted at $path.
**
**   Each row has the following columns:
**
**     name:  Path to file or directory (text value).
**     mode:  Value of stat.st_mode for directory entry (an integer).
**     mtime: Value of stat.st_mtime for directory entry (an integer).
**     data:  For a regular file, a blob containing the file data. For a
**            symlink, a text value containing the text of the link. For a
**            directory, NULL.
**     level: Directory hierarchy level.  Topmost is 1.
**
**   If a non-NULL value is specified for the optional $dir parameter and
**   $path is a relative path, then $path is interpreted relative to $dir. 
**   And the paths returned in the "name" column of the table are also 
**   relative to directory $dir.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if !defined(_WIN32) && !defined(WIN32)
#  include <unistd.h>
#  include <dirent.h>
#  include <utime.h>
#  include <sys/time.h>
#  define STRUCT_STAT struct stat
#  include <limits.h>
#  include <stdlib.h>
#else
#  include "windirent.h"
#  include <direct.h>
#  define STRUCT_STAT struct _stat
#  define chmod(path,mode) fileio_chmod(path,mode)
#  define mkdir(path,mode) fileio_mkdir(path)
   extern LPWSTR dbsql_win32_utf8_to_unicode(const char*);
   extern char *dbsql_win32_unicode_to_utf8(LPCWSTR);
#endif
#include <time.h>
#include <errno.h>

/* When used as part of the CLI, the dbsql_stdio.h module will have
** been included before this one. In that case use the dbsql_stdio.h
** #defines.  If not, create our own for fopen().
*/
#ifndef _DBSQL_STDIO_H_
# define dbsql_fopen fopen
#endif

/*
** Structure of the fsdir() table-valued function
*/
                 /*    0    1    2     3    4     5           6          */
#define FSDIR_SCHEMA "(name,mode,mtime,data,level,path HIDDEN,dir HIDDEN)"

#define FSDIR_COLUMN_NAME     0     /* Name of the file */
#define FSDIR_COLUMN_MODE     1     /* Access mode */
#define FSDIR_COLUMN_MTIME    2     /* Last modification time */
#define FSDIR_COLUMN_DATA     3     /* File content */
#define FSDIR_COLUMN_LEVEL    4     /* Level.  Topmost is 1 */
#define FSDIR_COLUMN_PATH     5     /* Path to top of search */
#define FSDIR_COLUMN_DIR      6     /* Path is relative to this directory */

/*
** UTF8 chmod() function for Windows
*/
#if defined(_WIN32) || defined(WIN32)
static int fileio_chmod(const char *path, int pmode){
  int rc;
  wchar_t *b1 = dbsql_win32_utf8_to_unicode(path);
  if( b1==0 ) return -1;
  rc = _wchmod(b1, pmode);
  dbsql_free(b1);
  return rc;
}
#endif

/*
** UTF8 mkdir() function for Windows
*/
#if defined(_WIN32) || defined(WIN32)
static int fileio_mkdir(const char *path){
  int rc;
  wchar_t *b1 = dbsql_win32_utf8_to_unicode(path);
  if( b1==0 ) return -1;
  rc = _wmkdir(b1);
  dbsql_free(b1);
  return rc;
}
#endif


/*
** Set the result stored by context ctx to a blob containing the 
** contents of file name.  Or, leave the result unchanged (NULL)
** if the file does not exist or is unreadable.
**
** If the file exceeds the dbsql blob size limit, through an
** DBSQL_TOOBIG error.
**
** Throw an DBSQL_IOERR if there are difficulties pulling the file
** off of disk.
*/
static void __read_file_contents(dbsql_func_t *ctx, const char *name){
  FILE *in;
  int64_t n_in;
  void *buf;
  DBSQL *db;
  int mx_blob;

  in = dbsql_fopen(name, "rb");
  if( in==0 ){
    /* File does not exist or is unreadable. Leave the result set to NULL. */
    return;
  }
  fseek(in, 0, SEEK_END);
  n_in = ftell(in);
  rewind(in);
  db = dbsql_context_db_handle(ctx);
  mx_blob = dbsql_limit(db, DBSQL_LIMIT_LENGTH, -1);
  if( n_in>mx_blob ){
    dbsql_result_error_code(ctx, DBSQL_TOOBIG);
    fclose(in);
    return;
  }
  buf = dbsql_malloc64( n_in ? n_in : 1 );
  if( buf==0 ){
    dbsql_result_error_nomem(ctx);
    fclose(in);
    return;
  }
  if( n_in==(int64_t)fread(buf, 1, (size_t)n_in, in) ){
    dbsql_result_blob64(ctx, buf, n_in, dbsql_free);
  }else{
    dbsql_result_error_code(ctx, DBSQL_IOERR);
    dbsql_free(buf);
  }
  fclose(in);
}

/*
** Implementation of the "readfile(X)" SQL function.  The entire content
** of the file named X is read and returned as a BLOB.  NULL is returned
** if the file does not exist or is unreadable.
*/
static void __readfile_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *name;
  (void)(argc);  /* Unused parameter */
  name = (const char*)dbsql_value_text(argv[0]);
  if( name==0 ) return;
  __read_file_contents(context, name);
}

/*
** Set the error message contained in context ctx to the results of
** vprintf(fmt, ...).
*/
static void __ctx_error_msg(dbsql_func_t *ctx, const char *fmt, ...){
  char *msg = 0;
  va_list ap;
  va_start(ap, fmt);
  msg = dbsql_vmprintf(fmt, ap);
  dbsql_result_error(ctx, msg, -1);
  dbsql_free(msg);
  va_end(ap);
}

#if defined(_WIN32)
/*
** This function is designed to convert a Win32 FILETIME structure into the
** number of seconds since the Unix Epoch (1970-01-01 00:00:00 UTC).
*/
static u_int64_t __file_time_to_unix_time(
  LPFILETIME file_time
){
  SYSTEMTIME epoch_system_time;
  ULARGE_INTEGER epoch_intervals;
  FILETIME epoch_file_time;
  ULARGE_INTEGER file_intervals;

  memset(&epoch_system_time, 0, sizeof(SYSTEMTIME));
  epoch_system_time.w_year = 1970;
  epoch_system_time.w_month = 1;
  epoch_system_time.w_day = 1;
  __system_time_to_file_time(&epoch_system_time, &epoch_file_time);
  epoch_intervals.__low_part = epoch_file_time.dw_low_date_time;
  epoch_intervals.__high_part = epoch_file_time.dw_high_date_time;

  file_intervals.__low_part = file_time->dw_low_date_time;
  file_intervals.__high_part = file_time->dw_high_date_time;

  return (file_intervals.__quad_part - epoch_intervals.__quad_part) / 10000000;
}
#endif /* _WIN32 */

/*
** This function is used in place of stat().  On Windows, special handling
** is required in order for the included time to be returned as UTC.  On all
** other systems, this function simply calls stat().
*/
static int __file_stat(
  const char *path,
  STRUCT_STAT *stat_buf
){
#if defined(_WIN32)
  int rc;
  wchar_t *b1 = dbsql_win32_utf8_to_unicode(path);
  if( b1==0 ) return 1;
  rc = _wstat(b1, stat_buf);
  if( rc==0 ){
    HANDLE h_find_file;
    WIN32_FIND_DATAW fd;
    memset(&fd, 0, sizeof(WIN32_FIND_DATAW));
    h_find_file = __find_first_file_w(b1, &fd);
    if( h_find_file!=NULL ){
      stat_buf->st_ctime = (time_t)__file_time_to_unix_time(&fd.ft_creation_time);
      stat_buf->st_atime = (time_t)__file_time_to_unix_time(&fd.ft_last_access_time);
      stat_buf->st_mtime = (time_t)__file_time_to_unix_time(&fd.ft_last_write_time);
      __find_close(h_find_file);
    }
  }
  dbsql_free(b1);
  return rc;
#else
  return stat(path, stat_buf);
#endif
}

/*
** This function is used in place of lstat().  On Windows, special handling
** is required in order for the included time to be returned as UTC.  On all
** other systems, this function simply calls lstat().
*/
static int __file_link_stat(
  const char *path,
  STRUCT_STAT *stat_buf
){
#if defined(_WIN32)
  return __file_stat(path, stat_buf);
#else
  return lstat(path, stat_buf);
#endif
}

/*
** Argument file is the name of a file that will be created and/or written
** by SQL function writefile(). This function ensures that the directory
** file will be written to exists, creating it if required. The permissions
** for any path components created by this function are set in accordance
** with the current umask.
**
** If an OOM condition is encountered, DBSQL_NOMEM is returned. Otherwise,
** DBSQL_OK is returned if the directory is successfully created, or
** DBSQL_ERROR otherwise.
*/
static int __make_directory(
  const char *file
){
  char *copy = dbsql_mprintf("%s", file);
  int rc = DBSQL_OK;

  if( copy==0 ){
    rc = DBSQL_NOMEM;
  }else{
    int n_copy = (int)strlen(copy);
    int i = 1;

    while( rc==DBSQL_OK ){
      STRUCT_STAT s_stat;
      int rc2;

      for(; copy[i]!='/' && i<n_copy; i++);
      if( i==n_copy ) break;
      copy[i] = '\0';

      rc2 = __file_stat(copy, &s_stat);
      if( rc2!=0 ){
        if( mkdir(copy, 0777) ) rc = DBSQL_ERROR;
      }else{
        if( !S_ISDIR(s_stat.st_mode) ) rc = DBSQL_ERROR;
      }
      copy[i] = '/';
      i++;
    }

    dbsql_free(copy);
  }

  return rc;
}

/*
** This function does the work for the writefile() UDF. Refer to 
** header comments at the top of this file for details.
*/
static int __write_file(
  dbsql_func_t *ctx,          /* Context to return bytes written in */
  const char *file,              /* File to write */
  dbsql_value_t *data,           /* Data to write */
  mode_t mode,                    /* MODE parameter passed to writefile() */
  int64_t mtime             /* MTIME parameter (or -1 to not set time) */
){
  if( file==0 ) return 1;
#if !defined(_WIN32) && !defined(WIN32)
  if( S_ISLNK(mode) ){
    const char *to = (const char*)dbsql_value_text(data);
    if( to==0 ) return 1;
    unlink(file);
    if( symlink(to, file)<0 ) return 1;
  }else
#endif
  {
    if( S_ISDIR(mode) ){
      if( mkdir(file, mode) ){
        /* The mkdir() call to create the directory failed. This might not
        ** be an error though - if there is already a directory at the same
        ** path and either the permissions already match or can be changed
        ** to do so using chmod(), it is not an error.  */
        STRUCT_STAT s_stat;
        if( errno!=EEXIST
         || 0!=__file_stat(file, &s_stat)
         || !S_ISDIR(s_stat.st_mode)
         || ((s_stat.st_mode&0777)!=(mode&0777) && 0!=chmod(file, mode&0777))
        ){
          return 1;
        }
      }
    }else{
      int64_t n_write = 0;
      const char *z;
      int rc = 0;
      FILE *out = dbsql_fopen(file, "wb");
      if( out==0 ) return 1;
      z = (const char*)dbsql_value_blob(data);
      if( z ){
        int64_t n = fwrite(z, 1, dbsql_value_bytes(data), out);
        n_write = dbsql_value_bytes(data);
        if( n_write!=n ){
          rc = 1;
        }
      }
      fclose(out);
      if( rc==0 && mode && chmod(file, mode & 0777) ){
        rc = 1;
      }
      if( rc ) return 2;
      dbsql_result_int64(ctx, n_write);
    }
  }

  if( mtime>=0 ){
#if defined(_WIN32)
    /* Windows */
    FILETIME last_access;
    FILETIME last_write;
    SYSTEMTIME current_time;
    LONGLONG intervals;
    HANDLE h_file;
    LPWSTR unicode_name;
    extern LPWSTR dbsql_win32_utf8_to_unicode(const char*);

    __get_system_time(&current_time);
    __system_time_to_file_time(&current_time, &last_access);
    intervals = (mtime*10000000) + 116444736000000000;
    last_write.dw_low_date_time = (DWORD)intervals;
    last_write.dw_high_date_time = intervals >> 32;
    unicode_name = dbsql_win32_utf8_to_unicode(file);
    if( unicode_name==0 ){
      return 1;
    }
    h_file = __create_file_w(
      unicode_name, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, NULL
    );
    dbsql_free(unicode_name);
    if( h_file!=INVALID_HANDLE_VALUE ){
      BOOL result = __set_file_time(h_file, NULL, &last_access, &last_write);
      __close_handle(h_file);
      return !result;
    }else{
      return 1;
    }
#elif defined(AT_FDCWD) && 0 /* utimensat() is not universally available */
    /* Recent unix */
    struct timespec times[2];
    times[0].tv_nsec = times[1].tv_nsec = 0;
    times[0].tv_sec = time(0);
    times[1].tv_sec = mtime;
    if( utimensat(AT_FDCWD, file, times, AT_SYMLINK_NOFOLLOW) ){
      return 1;
    }
#else
    /* Legacy unix. 
    **
    ** Do not use utimes() on a symbolic link - it sees through the link and
    ** modifies the timestamps on the target. Or fails if the target does 
    ** not exist.  */
    if( 0==S_ISLNK(mode) ){
      struct timeval times[2];
      times[0].tv_usec = times[1].tv_usec = 0;
      times[0].tv_sec = time(0);
      times[1].tv_sec = mtime;
      if( utimes(file, times) ){
        return 1;
      }
    }
#endif
  }

  return 0;
}

/*
** Implementation of the "writefile(W,X[,Y[,Z]]])" SQL function.  
** Refer to header comments at the top of this file for details.
*/
static void __writefile_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *file;
  mode_t mode = 0;
  int res;
  int64_t mtime = -1;

  if( argc<2 || argc>4 ){
    dbsql_result_error(context, 
        "wrong number of arguments to function writefile()", -1
    );
    return;
  }

  file = (const char*)dbsql_value_text(argv[0]);
  if( file==0 ) return;
  if( argc>=3 ){
    mode = (mode_t)dbsql_value_int(argv[2]);
  }
  if( argc==4 ){
    mtime = dbsql_value_int64(argv[3]);
  }

  res = __write_file(context, file, argv[1], mode, mtime);
  if( res==1 && errno==ENOENT ){
    if( __make_directory(file)==DBSQL_OK ){
      res = __write_file(context, file, argv[1], mode, mtime);
    }
  }

  if( argc>2 && res!=0 ){
    if( S_ISLNK(mode) ){
      __ctx_error_msg(context, "failed to create symlink: %s", file);
    }else if( S_ISDIR(mode) ){
      __ctx_error_msg(context, "failed to create directory: %s", file);
    }else{
      __ctx_error_msg(context, "failed to write file: %s", file);
    }
  }
}

/*
** SQL function:   lsmode(MODE)
**
** Given a numberic st_mode from stat(), convert it into a human-readable
** text string in the style of "ls -l".
*/
static void __ls_mode_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int i;
  int i_mode = dbsql_value_int(argv[0]);
  char z[16];
  (void)argc;
  if( S_ISLNK(i_mode) ){
    z[0] = 'l';
  }else if( S_ISREG(i_mode) ){
    z[0] = '-';
  }else if( S_ISDIR(i_mode) ){
    z[0] = 'd';
  }else{
    z[0] = '?';
  }
  for(i=0; i<3; i++){
    int m = (i_mode >> ((2-i)*3));
    char *a = &z[1 + i*3];
    a[0] = (m & 0x4) ? 'r' : '-';
    a[1] = (m & 0x2) ? 'w' : '-';
    a[2] = (m & 0x1) ? 'x' : '-';
  }
  z[10] = '\0';
  dbsql_result_text(context, z, -1, DBSQL_TRANSIENT);
}

#ifndef DBSQL_OMIT_VIRTUALTABLE

/* 
** Cursor type for recursively iterating through a directory structure.
*/
typedef struct fsdir_cursor fsdir_cursor;
typedef struct fsdir_level_t fsdir_level_t;

struct fsdir_level_t {
  DIR *dir;                 /* From opendir() */
  char *z_dir;                /* Name of directory (nul-terminated) */
};

struct fsdir_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */

  int n_lvl;                  /* Number of entries in lvls[] array */
  int mx_lvl;                 /* Maximum level */
  int i_lvl;                  /* index_t of current entry */
  fsdir_level_t *lvls;          /* Hierarchy of directories being traversed */

  const char *z_base;
  int n_base;

  STRUCT_STAT s_stat;         /* Current lstat() results */
  char *path;               /* Path to current entry */
  int64_t i_rowid;      /* Current rowid */
};

typedef struct fsdir_tab fsdir_tab;
struct fsdir_tab {
  dbsql_vtab_t base;         /* Base class - must be first */
};

/*
** Construct a new fsdir virtual table object.
*/
static int __fsdir_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  fsdir_tab *p_new = 0;
  int rc;
  (void)aux;
  (void)argc;
  (void)argv;
  (void)err_p;
  rc = dbsql_declare_vtab(db, "CREATE TABLE x" FSDIR_SCHEMA);
  if( rc==DBSQL_OK ){
    p_new = (fsdir_tab*)dbsql_malloc64( sizeof(*p_new) );
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
    dbsql_vtab_config(db, DBSQL_VTAB_DIRECTONLY);
  }
  *vtab = (dbsql_vtab_t*)p_new;
  return rc;
}

/*
** This method is the destructor for fsdir vtab objects.
*/
static int __fsdir_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Constructor for a new fsdir_cursor object.
*/
static int __fsdir_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  fsdir_cursor *cur;
  (void)p;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->i_lvl = -1;
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Reset a cursor back to the state it was in when first returned
** by __fsdir_open().
*/
static void __fsdir_reset_cursor(fsdir_cursor *cur){
  int i;
  for(i=0; i<=cur->i_lvl; i++){
    fsdir_level_t *lvl = &cur->lvls[i];
    if( lvl->dir ) closedir(lvl->dir);
    dbsql_free(lvl->z_dir);
  }
  dbsql_free(cur->path);
  dbsql_free(cur->lvls);
  cur->lvls = 0;
  cur->path = 0;
  cur->z_base = 0;
  cur->n_base = 0;
  cur->n_lvl = 0;
  cur->i_lvl = -1;
  cur->i_rowid = 1;
}

/*
** Destructor for an fsdir_cursor.
*/
static int __fsdir_close(dbsql_vtab_cursor_t *cur){
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;

  __fsdir_reset_cursor(p_cur);
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Set the error message for the virtual table associated with cursor
** cur to the results of vprintf(fmt, ...).
*/
static void __fsdir_set_errmsg(fsdir_cursor *cur, const char *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  cur->base.vtab->errmsg = dbsql_vmprintf(fmt, ap);
  va_end(ap);
}


/*
** Advance an fsdir_cursor to its next row of output.
*/
static int __fsdir_next(dbsql_vtab_cursor_t *cur){
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;
  mode_t m = p_cur->s_stat.st_mode;

  p_cur->i_rowid++;
  if( S_ISDIR(m) && p_cur->i_lvl+3<p_cur->mx_lvl ){
    /* Descend into this directory */
    int i_new = p_cur->i_lvl + 1;
    fsdir_level_t *lvl;
    if( i_new>=p_cur->n_lvl ){
      int n_new = i_new+1;
      int64_t n_byte = n_new*sizeof(fsdir_level_t);
      fsdir_level_t *news = (fsdir_level_t*)dbsql_realloc64(p_cur->lvls, n_byte);
      if( news==0 ) return DBSQL_NOMEM;
      memset(&news[p_cur->n_lvl], 0, sizeof(fsdir_level_t)*(n_new-p_cur->n_lvl));
      p_cur->lvls = news;
      p_cur->n_lvl = n_new;
    }
    p_cur->i_lvl = i_new;
    lvl = &p_cur->lvls[i_new];
    
    lvl->z_dir = p_cur->path;
    p_cur->path = 0;
    lvl->dir = opendir(lvl->z_dir);
    if( lvl->dir==0 ){
      __fsdir_set_errmsg(p_cur, "cannot read directory: %s", lvl->z_dir);
      return DBSQL_ERROR;
    }
  }

  while( p_cur->i_lvl>=0 ){
    fsdir_level_t *lvl = &p_cur->lvls[p_cur->i_lvl];
    struct dirent *entry = readdir(lvl->dir);
    if( entry ){
      if( entry->d_name[0]=='.' ){
       if( entry->d_name[1]=='.' && entry->d_name[2]=='\0' ) continue;
       if( entry->d_name[1]=='\0' ) continue;
      }
      dbsql_free(p_cur->path);
      p_cur->path = dbsql_mprintf("%s/%s", lvl->z_dir, entry->d_name);
      if( p_cur->path==0 ) return DBSQL_NOMEM;
      if( __file_link_stat(p_cur->path, &p_cur->s_stat) ){
        __fsdir_set_errmsg(p_cur, "cannot stat file: %s", p_cur->path);
        return DBSQL_ERROR;
      }
      return DBSQL_OK;
    }
    closedir(lvl->dir);
    dbsql_free(lvl->z_dir);
    lvl->dir = 0;
    lvl->z_dir = 0;
    p_cur->i_lvl--;
  }

  /* EOF */
  dbsql_free(p_cur->path);
  p_cur->path = 0;
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the series_cursor
** is currently pointing.
*/
static int __fsdir_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;
  switch( i ){
    case FSDIR_COLUMN_NAME: {
      dbsql_result_text(ctx, &p_cur->path[p_cur->n_base], -1, DBSQL_TRANSIENT);
      break;
    }

    case FSDIR_COLUMN_MODE:
      dbsql_result_int64(ctx, p_cur->s_stat.st_mode);
      break;

    case FSDIR_COLUMN_MTIME:
      dbsql_result_int64(ctx, p_cur->s_stat.st_mtime);
      break;

    case FSDIR_COLUMN_DATA: {
      mode_t m = p_cur->s_stat.st_mode;
      if( S_ISDIR(m) ){
        dbsql_result_null(ctx);
#if !defined(_WIN32) && !defined(WIN32)
      }else if( S_ISLNK(m) ){
        char statics[64];
        char *bufs = statics;
        int64_t n_buf = 64;
        int n;

        while( 1 ){
          n = readlink(p_cur->path, bufs, n_buf);
          if( n<n_buf ) break;
          if( bufs!=statics ) dbsql_free(bufs);
          n_buf = n_buf*2;
          bufs = dbsql_malloc64(n_buf);
          if( bufs==0 ){
            dbsql_result_error_nomem(ctx);
            return DBSQL_NOMEM;
          }
        }

        if( n>0 ){
          dbsql_result_text(ctx, bufs, n, DBSQL_TRANSIENT);
        }else{
          dbsql_result_null(ctx);
        }
        if( bufs!=statics ) dbsql_free(bufs);
#endif
      }else{
        __read_file_contents(ctx, p_cur->path);
      }
      break;
    }
    case FSDIR_COLUMN_LEVEL:
      dbsql_result_int(ctx, p_cur->i_lvl+2);
      break;
    case FSDIR_COLUMN_PATH:
    default: {
      /* The FSDIR_COLUMN_PATH and FSDIR_COLUMN_DIR are input parameters.
      ** always return their values as NULL */
      break;
    }
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row. In this implementation, the
** first row returned is assigned rowid value 1, and each subsequent
** row a value 1 more than that of the previous.
*/
static int __fsdir_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __fsdir_eof(dbsql_vtab_cursor_t *cur){
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;
  return (p_cur->path==0);
}

/*
** filter callback.
**
** idx_num bit      Meaning
**     0x01         PATH=N
**     0x02         DIR=N
**     0x04         LEVEL<N
**     0x08         LEVEL<=N  
*/
static int __fsdir_filter(
  dbsql_vtab_cursor_t *cur, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  const char *dir = 0;
  fsdir_cursor *p_cur = (fsdir_cursor*)cur;
  int i;
  (void)idx_str;
  __fsdir_reset_cursor(p_cur);

  if( idx_num==0 ){
    __fsdir_set_errmsg(p_cur, "table function fsdir requires an argument");
    return DBSQL_ERROR;
  }

  assert( (idx_num & 0x01)!=0 && argc>0 );
  dir = (const char*)dbsql_value_text(argv[0]);
  if( dir==0 ){
    __fsdir_set_errmsg(p_cur, "table function fsdir requires a non-NULL argument");
    return DBSQL_ERROR;
  }
  i = 1;
  if( (idx_num & 0x02)!=0 ){
    assert( argc>i );
    p_cur->z_base = (const char*)dbsql_value_text(argv[i++]);
  }
  if( (idx_num & 0x0c)!=0 ){
    assert( argc>i );
    p_cur->mx_lvl = dbsql_value_int(argv[i++]);
    if( idx_num & 0x08 ) p_cur->mx_lvl++;
    if( p_cur->mx_lvl<=0 ) p_cur->mx_lvl = 1000000000;
  }else{
    p_cur->mx_lvl = 1000000000;
  }
  if( p_cur->z_base ){
    p_cur->n_base = (int)strlen(p_cur->z_base)+1;
    p_cur->path = dbsql_mprintf("%s/%s", p_cur->z_base, dir);
  }else{
    p_cur->path = dbsql_mprintf("%s", dir);
  }

  if( p_cur->path==0 ){
    return DBSQL_NOMEM;
  }
  if( __file_link_stat(p_cur->path, &p_cur->s_stat) ){
    __fsdir_set_errmsg(p_cur, "cannot stat file: %s", p_cur->path);
    return DBSQL_ERROR;
  }

  return DBSQL_OK;
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the generate_series virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
**
** In this implementation idx_num is used to represent the
** query plan.  idx_str is unused.
**
** The query plan is represented by bits in idx_num:
**
**  0x01  The path value is supplied by argv[0]
**  0x02  dir is in argv[1]
**  0x04  maxdepth is in argv[1] or [2]
*/
static int __fsdir_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;                 /* Loop over constraints */
  int idx_path = -1;      /* index_t in idx_info->constraints of PATH= */
  int idx_dir = -1;       /* index_t in idx_info->constraints of DIR= */
  int idx_level = -1;     /* index_t in idx_info->constraints of LEVEL< or <= */
  int idx_level_eq = 0;    /* 0x08 for LEVEL<= or LEVEL=.  0x04 for LEVEL< */
  int omit_level = 0;     /* omit the LEVEL constraint */
  int seen_path = 0;      /* True if an unusable PATH= constraint is seen */
  int seen_dir = 0;       /* True if an unusable DIR= constraint is seen */
  const struct dbsql_index_constraint_t *constraint;

  (void)tab;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->op==DBSQL_INDEX_CONSTRAINT_EQ ){
      switch( constraint->i_column ){
        case FSDIR_COLUMN_PATH: {
          if( constraint->usable ){
            idx_path = i;
            seen_path = 0;
          }else if( idx_path<0 ){
            seen_path = 1;
          }
          break;
        }
        case FSDIR_COLUMN_DIR: {
          if( constraint->usable ){
            idx_dir = i;
            seen_dir = 0;
          }else if( idx_dir<0 ){
            seen_dir = 1;
          }
          break;
        }
        case FSDIR_COLUMN_LEVEL: {
          if( constraint->usable && idx_level<0 ){
            idx_level = i;
            idx_level_eq = 0x08;
            omit_level = 0;
          }
          break;
        }
      }
    }else
    if( constraint->i_column==FSDIR_COLUMN_LEVEL
     && constraint->usable
     && idx_level<0
    ){
      if( constraint->op==DBSQL_INDEX_CONSTRAINT_LE ){
        idx_level = i;
        idx_level_eq = 0x08;
        omit_level = 1;
      }else if( constraint->op==DBSQL_INDEX_CONSTRAINT_LT ){
        idx_level = i;
        idx_level_eq = 0x04;
        omit_level = 1;
      }
    } 
  }
  if( seen_path || seen_dir ){
    /* If input parameters are unusable, disallow this plan */
    return DBSQL_CONSTRAINT;
  }

  if( idx_path<0 ){
    idx_info->idx_num = 0;
    /* The idx_info->estimated_cost should have been initialized to a huge
    ** number.  Leave it unchanged. */
    idx_info->estimated_rows = 0x7fffffff;
  }else{
    idx_info->constraint_usages[idx_path].omit = 1;
    idx_info->constraint_usages[idx_path].argv_index = 1;
    idx_info->idx_num = 0x01;
    idx_info->estimated_cost = 1.0e9;
    i = 2;
    if( idx_dir>=0 ){
      idx_info->constraint_usages[idx_dir].omit = 1;
      idx_info->constraint_usages[idx_dir].argv_index = i++;
      idx_info->idx_num |= 0x02;
      idx_info->estimated_cost /= 1.0e4;
    }
    if( idx_level>=0 ){
      idx_info->constraint_usages[idx_level].omit = omit_level;
      idx_info->constraint_usages[idx_level].argv_index = i++;
      idx_info->idx_num |= idx_level_eq;
      idx_info->estimated_cost /= 1.0e4;
    }
  }

  return DBSQL_OK;
}

/*
** Register the "fsdir" virtual table.
*/
static int FSDIR_REGISTER(DBSQL *db){
  static dbsql_module_t __fsdir_module = {
    0,                         /* i_version */
    0,                         /* create */
    __fsdir_connect,              /* connect */
    __fsdir_best_index,            /* best_index */
    __fsdir_disconnect,           /* disconnect */
    0,                         /* destroy */
    __fsdir_open,                 /* x_open - open a cursor */
    __fsdir_close,                /* x_close - close a cursor */
    __fsdir_filter,               /* filter - configure scan constraints */
    __fsdir_next,                 /* next - advance a cursor */
    __fsdir_eof,                  /* eof - check for end of scan */
    __fsdir_column,               /* column - read data */
    __fsdir_rowid,                /* rowid - read data */
    0,                         /* update */
    0,                         /* begin */
    0,                         /* x_sync */
    0,                         /* commit */
    0,                         /* rollback */
    0,                         /* find_method */
    0,                         /* rename */
    0,                         /* savepoint */
    0,                         /* release */
    0,                         /* rollback_to */
    0,                         /* shadow_name */
    0                          /* integrity */
  };

  int rc = dbsql_create_module(db, "fsdir", &__fsdir_module, 0);
  return rc;
}
#else         /* DBSQL_OMIT_VIRTUALTABLE */
# define FSDIR_REGISTER(x) DBSQL_OK
#endif

/*
** This version of realpath() works on any system.  The string
** returned is held in memory allocated using dbsql_malloc64().
** The caller is responsible for calling dbsql_free().
*/
static char *portable_realpath(const char *path){
#if !defined(_WIN32)       /* BEGIN unix */

  char *out = 0;          /* Result */
  char *z;                 /* Temporary buffer */
#if defined(PATH_MAX)
  char buf[PATH_MAX+1];   /* Space for the temporary buffer */
#endif

  if( path==0 ) return 0;
#if defined(PATH_MAX)
  z = realpath(path, buf);
  if( z ){
    out = dbsql_mprintf("%s", buf);
  }
#endif /* defined(PATH_MAX) */
  if( out==0 ){
    /* Try POSIX.1-2008 malloc behavior */
    z = realpath(path, NULL);
    if( z ){
      out = dbsql_mprintf("%s", z);
      free(z);
    }
  }
  return out;

#else /* End UNIX, Begin WINDOWS */

  wchar_t *path16;        /* UTF16 translation of path */
  char *out = 0;          /* Result */
  wchar_t *z = 0;          /* Temporary buffer */

  if( path==0 ) return 0;

  path16 = dbsql_win32_utf8_to_unicode(path);
  if( path16==0 ) return 0;
  z = _wfullpath(NULL, path16, 0);
  dbsql_free(path16);
  if( z ){
    out = dbsql_win32_unicode_to_utf8(z);
    free(z);
  }
  return out;

#endif /* End WINDOWS, Begin common code */
}

/*
** SQL function:   realpath(X)
**
** Try to convert file or pathname X into its real, absolute pathname.
** Return NULL if unable.
**
** The file or directory X is not required to exist.  The answer is formed
** by calling system realpath() on the prefix of X that does exist and
** appending the tail of X that does not (yet) exist.
*/
static void __realpath_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *path;    /* Original input path */
  char *copy;          /* An editable copy of path */
  char *out;           /* The result */
  char c_sep = 0;        /* Separator turned into \000 */
  size_t len;           /* Prefix length before c_sep */
#ifdef _WIN32
  const int is_win = 1;
#else
  const int is_win = 0;
#endif

  (void)argc;
  path = (const char*)dbsql_value_text(argv[0]);
  if( path==0 ) return;
  if( path[0]==0 ) path = ".";
  copy = dbsql_mprintf("%s",path);
  len = strlen(copy);
  while( len>1 && (copy[len-1]=='/' || (is_win && copy[len-1]=='\\')) ){
    len--;
  }
  copy[len] = 0;
  while( 1 /*exit-by-break*/ ){
    out = portable_realpath(copy);
    copy[len] = c_sep;
    if( out ){
      if( c_sep ){
        out = dbsql_mprintf("%z%s",out,&copy[len]);
      }
      break;
    }else{
      size_t i = len-1;
      while( i>0 ){
        if( copy[i]=='/' || (is_win && copy[i]=='\\') ) break;
        i--;
      }
      if( i<=0 ){
        if( copy[0]=='/' ){
          out = copy;
          copy = 0;
        }else if( (out = portable_realpath("."))!=0 ){
          out = dbsql_mprintf("%z/%s", out, copy);
        }          
        break;
      }
      c_sep = copy[i];
      copy[i] = 0;
      len = i;
    }
  }
  dbsql_free(copy);
  if( out ){
    /* Simplify any "/./" or "/../" that might have snuck into the
    ** pathname due to appending of copy.  We only have to consider
    ** unix "/" separators, because the _wfilepath() system call on
    ** Windows will have already done this simplification for us. */
    size_t i, j, n;
    n = strlen(out);
    for(i=j=0; i<n; i++){
      if( out[i]=='/' ){
        if( out[i+1]=='/' ) continue;
        if( out[i+1]=='.' && i+2<n && out[i+2]=='/' ){
          i += 1;
          continue;
        }
        if( out[i+1]=='.' && i+3<n && out[i+2]=='.' && out[i+3]=='/' ){
          while( j>0 && out[j-1]!='/' ){ j--; }
          if( j>0 ){ j--; }
          i += 2;
          continue;
        }
      }
      out[j++] = out[i];
    }
    out[j] = 0;

    /* Return the result */
    dbsql_result_text(context, out, -1, dbsql_free);
  }
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_fileio_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "readfile", 1, 
                               DBSQL_UTF8|DBSQL_DIRECTONLY, 0,
                               __readfile_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "writefile", -1,
                                 DBSQL_UTF8|DBSQL_DIRECTONLY, 0,
                                 __writefile_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "lsmode", 1, DBSQL_UTF8, 0,
                                 __ls_mode_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = FSDIR_REGISTER(db);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "realpath", 1,
                                 DBSQL_UTF8, 0,
                                 __realpath_func, 0, 0);
  }
  return rc;
}
