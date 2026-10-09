/*
** 2013 Jan 11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table interfaces.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library.
**
** The FS virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE tbl USING fs(idx);
**
** where idx is the name of a table in the db with 2 columns.  The virtual
** table also has two columns - file path and file contents.
**
** The first column of table idx must be an IPK, and the second contains file
** paths. For example:
**
**   CREATE TABLE idx(id INTEGER PRIMARY KEY, path TEXT);
**   INSERT INTO idx VALUES(4, '/etc/passwd');
**
** Adding the row to the idx table automatically creates a row in the 
** virtual table with rowid=4, path=/etc/passwd and a text field that 
** contains data read from file /etc/passwd on disk.
**
*************************************************************************
** Virtual table module "fsdir"
**
** This module is designed to be used as a read-only eponymous virtual table.
** Its schema is as follows:
**
**   CREATE TABLE fsdir(dir TEXT, name TEXT);
**
** When queried, a WHERE term of the form "dir = $dir" must be provided. The
** virtual table then appears to have one row for each entry in file-system
** directory $dir. column_t dir contains a copy of $dir, and column "name"
** contains the name of the directory entry.
**
** If the specified $dir cannot be opened or is not a directory, it is not
** an error. The virtual table appears to be empty in this case.
**
*************************************************************************
** Virtual table module "fstree"
**
** This module is also a read-only eponymous virtual table with the 
** following schema:
**
**   CREATE TABLE fstree(path TEXT, size INT, data BLOB);
**
** Running a "SELECT * FROM fstree" query on this table returns the entire
** contents of the file-system, starting at "/". To restrict the search
** space, the virtual table supports LIKE and GLOB constraints on the
** 'path' column. For example:
**
**   SELECT * FROM fstree WHERE path LIKE '/home/dan/sqlite/%'
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if !defined(_WIN32) || defined(__MSVCRT__)
# include <unistd.h>
# include <dirent.h>
#else
# include "windirent.h"
# ifndef S_ISREG
#  define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
# endif
#endif

#ifndef DBSQL_OMIT_VIRTUALTABLE

typedef struct fs_vtab fs_vtab;
typedef struct fs_cursor fs_cursor;

/* 
** A fs virtual-table object 
*/
struct fs_vtab {
  dbsql_vtab_t base;
  DBSQL *db;
  char *z_db;                      /* Name of db containing tbl */
  char *tbl;                     /* Name of docid->file map table */
};

/* A fs cursor object */
struct fs_cursor {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *stmt;
  char *buf;
  int n_buf;
  int n_alloc;
};

/*************************************************************************
** Start of fsdir implementation.
*/
typedef struct fsdir_vtab_t fsdir_vtab_t;
typedef struct fsdir_csr_t fsdir_csr_t;
struct fsdir_vtab_t {
  dbsql_vtab_t base;
};

struct fsdir_csr_t {
  dbsql_vtab_cursor_t base;
  char *z_dir;                     /* Buffer containing directory scanned */
  DIR *dir;                      /* Open directory */
  int64_t i_rowid;
  struct dirent *entry;
};

/*
** This function is the implementation of both the connect and create
** methods of the fsdir virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int __fsdir_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  fsdir_vtab_t *tab;

  if( argc!=3 ){
    *err_p = dbsql_mprintf("wrong number of arguments");
    return DBSQL_ERROR;
  }

  tab = (fsdir_vtab_t *)dbsql_malloc(sizeof(fsdir_vtab_t));
  if( !tab ) return DBSQL_NOMEM;
  memset(tab, 0, sizeof(fsdir_vtab_t));

  *vtab = &tab->base;
  dbsql_declare_vtab(db, "CREATE TABLE xyz(dir, name);");

  return DBSQL_OK;
}

/*
** destroy/disconnect implementation.
*/
static int __fsdir_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** best_index implementation. The only constraint supported is:
**
**   (dir = ?)
*/
static int __fsdir_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int ii;

  idx_info->estimated_cost = 1000000000.0;

  for(ii=0; ii<idx_info->n_constraint; ii++){
    struct dbsql_index_constraint_t const *p = &idx_info->constraints[ii];
    if( p->i_column==0 && p->usable && p->op==DBSQL_INDEX_CONSTRAINT_EQ ){
      struct dbsql_index_constraint_usage_t *usage;
      usage = &idx_info->constraint_usages[ii];
      usage->omit = 1;
      usage->argv_index = 1;
      idx_info->idx_num = 1;
      idx_info->estimated_cost = 1.0;
      break;
    }
  }

  return DBSQL_OK;
}

/*
** x_open implementation.
**
** Open a new fsdir cursor.
*/
static int __fsdir_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  fsdir_csr_t *cur;
  /* Allocate an extra 256 bytes because it is undefined how big dirent.d_name
  ** is and we need enough space.  Linux provides plenty already, but
  ** Solaris only provides one byte. */
  cur = (fsdir_csr_t*)dbsql_malloc(sizeof(fsdir_csr_t)+256);
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(fsdir_csr_t));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Close a fsdir cursor.
*/
static int __fsdir_close(dbsql_vtab_cursor_t *cur){
  fsdir_csr_t *p_cur = (fsdir_csr_t*)cur;
  if( p_cur->dir ) closedir(p_cur->dir);
  dbsql_free(p_cur->z_dir);
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Skip the cursor to the next entry.
*/
static int __fsdir_next(dbsql_vtab_cursor_t *cur){
  fsdir_csr_t *csr = (fsdir_csr_t*)cur;

  if( csr->dir ){
    csr->entry = readdir(csr->dir);
    if( csr->entry==0 ){
      closedir(csr->dir);
      csr->dir = 0;
    }
    csr->i_rowid++;
  }

  return DBSQL_OK;
}

/*
** filter method implementation.
*/
static int __fsdir_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  fsdir_csr_t *csr = (fsdir_csr_t*)vtab_cursor;
  const char *dir;
  int n_dir;


  if( idx_num!=1 || argc!=1 ){
    return DBSQL_ERROR;
  }

  csr->i_rowid = 0;
  dbsql_free(csr->z_dir);
  if( csr->dir ){
    closedir(csr->dir);
    csr->dir = 0;
  }

  dir = (const char*)dbsql_value_text(argv[0]);
  n_dir = dbsql_value_bytes(argv[0]);
  csr->z_dir = dbsql_malloc(n_dir+1);
  if( csr->z_dir==0 ) return DBSQL_NOMEM;
  memcpy(csr->z_dir, dir, n_dir+1);

  csr->dir = opendir(csr->z_dir);
  return __fsdir_next(vtab_cursor); 
}

/*
** eof method implementation.
*/
static int __fsdir_eof(dbsql_vtab_cursor_t *cur){
  fsdir_csr_t *csr = (fsdir_csr_t*)cur;
  return csr->dir==0;
}

/*
** column method implementation.
*/
static int __fsdir_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  fsdir_csr_t *csr = (fsdir_csr_t*)cur;
  switch( i ){
    case 0: /* dir */
      dbsql_result_text(ctx, csr->z_dir, -1, DBSQL_STATIC);
      break;

    case 1: /* name */
      dbsql_result_text(ctx, csr->entry->d_name, -1, DBSQL_TRANSIENT);
      break;

    default:
      assert( 0 );
  }

  return DBSQL_OK;
}

/*
** rowid method implementation.
*/
static int __fsdir_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  fsdir_csr_t *csr = (fsdir_csr_t*)cur;
  *rowid = csr->i_rowid;
  return DBSQL_OK;
}
/*
** End of fsdir implementation.
*************************************************************************/

/*************************************************************************
** Start of fstree implementation.
*/
typedef struct fstree_vtab_t fstree_vtab_t;
typedef struct fstree_csr_t fstree_csr_t;
struct fstree_vtab_t {
  dbsql_vtab_t base;
  DBSQL *db;
};

struct fstree_csr_t {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *stmt;            /* Statement to list paths */
  int fd;                         /* File descriptor open on current path */
};

/*
** This function is the implementation of both the connect and create
** methods of the fstree virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int __fstree_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  fstree_vtab_t *tab;

  if( argc!=3 ){
    *err_p = dbsql_mprintf("wrong number of arguments");
    return DBSQL_ERROR;
  }

  tab = (fstree_vtab_t *)dbsql_malloc(sizeof(fstree_vtab_t));
  if( !tab ) return DBSQL_NOMEM;
  memset(tab, 0, sizeof(fstree_vtab_t));
  tab->db = db;

  *vtab = &tab->base;
  dbsql_declare_vtab(db, "CREATE TABLE xyz(path, size, data);");

  return DBSQL_OK;
}

/*
** destroy/disconnect implementation.
*/
static int __fstree_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** best_index implementation. The only constraint supported is:
**
**   (dir = ?)
*/
static int __fstree_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int ii;

  for(ii=0; ii<idx_info->n_constraint; ii++){
    struct dbsql_index_constraint_t const *p = &idx_info->constraints[ii];
    if( p->i_column==0 && p->usable && (
          p->op==DBSQL_INDEX_CONSTRAINT_GLOB
       || p->op==DBSQL_INDEX_CONSTRAINT_LIKE
       || p->op==DBSQL_INDEX_CONSTRAINT_EQ
    )){
      struct dbsql_index_constraint_usage_t *usage;
      usage = &idx_info->constraint_usages[ii];
      idx_info->idx_num = p->op;
      usage->argv_index = 1;
      idx_info->estimated_cost = 100000.0;
      return DBSQL_OK;
    }
  }

  idx_info->estimated_cost = 1000000000.0;
  return DBSQL_OK;
}

/*
** x_open implementation.
**
** Open a new fstree cursor.
*/
static int __fstree_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  fstree_csr_t *cur;
  cur = (fstree_csr_t*)dbsql_malloc(sizeof(fstree_csr_t));
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(fstree_csr_t));
  cur->fd = -1;
  *cursor = &cur->base;
  return DBSQL_OK;
}

static void __fstree_close_fd(fstree_csr_t *csr){
  if( csr->fd>=0 ){
    close(csr->fd);
    csr->fd = -1;
  }
}

/*
** Close a fstree cursor.
*/
static int __fstree_close(dbsql_vtab_cursor_t *cur){
  fstree_csr_t *csr = (fstree_csr_t*)cur;
  dbsql_finalize(csr->stmt);
  __fstree_close_fd(csr);
  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** Skip the cursor to the next entry.
*/
static int __fstree_next(dbsql_vtab_cursor_t *cur){
  fstree_csr_t *csr = (fstree_csr_t*)cur;
  int rc;

  __fstree_close_fd(csr);
  rc = dbsql_step(csr->stmt);
  if( rc!=DBSQL_ROW ){
    rc = dbsql_finalize(csr->stmt);
    csr->stmt = 0;
  }else{
    rc = DBSQL_OK;
    csr->fd = open((const char*)dbsql_column_text(csr->stmt, 0), O_RDONLY);
  }

  return rc;
}

/*
** filter method implementation.
*/
static int __fstree_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  fstree_csr_t *csr = (fstree_csr_t*)vtab_cursor;
  fstree_vtab_t *tab = (fstree_vtab_t*)(csr->base.vtab);
  int rc;
  const char *sql = 
"WITH r(d) AS ("
"  SELECT CASE WHEN dir=?2 THEN ?3 ELSE dir END || '/' || name "
"    FROM fsdir WHERE dir=?1 AND name NOT LIKE '.%'"
"  UNION ALL"
"  SELECT dir || '/' || name FROM r, fsdir WHERE dir=d AND name NOT LIKE '.%'"
") SELECT d FROM r;";

  char *root;
  int n_root;
  char *prefix;
  int n_prefix;
  const char *dir;
  int n_dir;
  char wilds[2] = { '\0', '\0' };

#ifdef _WIN32
  const char *drive = getenv("fstreeDrive");
  if( drive==0 ){
    drive = getenv("SystemDrive");
  }
  root = dbsql_mprintf("%s%c", drive, '/');
  n_root = __strlen30(root);
  prefix = dbsql_mprintf("%s", drive);
  n_prefix = __strlen30(prefix);
#else
  root = "/";
  n_root = 1;
  prefix = "";
  n_prefix = 0;
#endif

  dir = root;
  n_dir = n_root;

  __fstree_close_fd(csr);
  dbsql_finalize(csr->stmt);
  csr->stmt = 0;
  rc = dbsql_prepare_v2(tab->db, sql, -1, &csr->stmt, 0);
  if( rc!=DBSQL_OK ) return rc;

  if( idx_num ){
    const char *query = (const char*)dbsql_value_text(argv[0]);
    switch( idx_num ){
      case DBSQL_INDEX_CONSTRAINT_GLOB:
        wilds[0] = '*';
        wilds[1] = '?';
        break;
      case DBSQL_INDEX_CONSTRAINT_LIKE:
        wilds[0] = '_';
        wilds[1] = '%';
        break;
    }

    if( dbsql_strnicmp(query, prefix, n_prefix)==0 ){
      int i;
      for(i=n_prefix; query[i]; i++){
        if( query[i]==wilds[0] || query[i]==wilds[1] ) break;
        if( query[i]=='/' ) n_dir = i;
      }
      dir = query;
    }
  }
  if( n_dir==0 ) n_dir = 1;

  dbsql_bind_text(csr->stmt, 1, dir, n_dir, DBSQL_TRANSIENT);
  dbsql_bind_text(csr->stmt, 2, root, n_root, DBSQL_TRANSIENT);
  dbsql_bind_text(csr->stmt, 3, prefix, n_prefix, DBSQL_TRANSIENT);

#ifdef _WIN32
  dbsql_free(prefix);
  dbsql_free(root);
#endif

  return __fstree_next(vtab_cursor); 
}

/*
** eof method implementation.
*/
static int __fstree_eof(dbsql_vtab_cursor_t *cur){
  fstree_csr_t *csr = (fstree_csr_t*)cur;
  return csr->stmt==0;
}

/*
** column method implementation.
*/
static int __fstree_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  fstree_csr_t *csr = (fstree_csr_t*)cur;
  if( i==0 ){      /* path */
    dbsql_result_value(ctx, dbsql_column_value(csr->stmt, 0));
  }else{
    struct stat s_buf;
    fstat(csr->fd, &s_buf);

    if( S_ISREG(s_buf.st_mode) ){
      if( i==1 ){
        dbsql_result_int64(ctx, s_buf.st_size);
      }else{
        int n_read;
        char *bufs = dbsql_malloc(s_buf.st_mode+1);
        if( !bufs ) return DBSQL_NOMEM;
        n_read = read(csr->fd, bufs, s_buf.st_mode);
        if( n_read!=s_buf.st_mode ){
          return DBSQL_IOERR;
        }
        dbsql_result_blob(ctx, bufs, n_read, DBSQL_TRANSIENT);
        dbsql_free(bufs);
      }
    }
  }

  return DBSQL_OK;
}

/*
** rowid method implementation.
*/
static int __fstree_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  *rowid = 0;
  return DBSQL_OK;
}
/*
** End of fstree implementation.
*************************************************************************/




/*
** This function is the implementation of both the connect and create
** methods of the fs virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int __fs_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  fs_vtab *vtab;
  int n_byte;
  const char *tbl;
  const char *z_db = argv[1];

  if( argc!=4 ){
    *err_p = dbsql_mprintf("wrong number of arguments");
    return DBSQL_ERROR;
  }
  tbl = argv[3];

  n_byte = sizeof(fs_vtab) + (int)strlen(tbl) + 1 + (int)strlen(z_db) + 1;
  vtab = (fs_vtab *)__malloc_zero( n_byte );
  if( !vtab ) return DBSQL_NOMEM;

  vtab->tbl = (char *)&vtab[1];
  vtab->z_db = &vtab->tbl[strlen(tbl)+1];
  vtab->db = db;
  memcpy(vtab->tbl, tbl, strlen(tbl));
  memcpy(vtab->z_db, z_db, strlen(z_db));
  *pp_vtab = &vtab->base;
  dbsql_declare_vtab(db, "CREATE TABLE x(path TEXT, data TEXT)");

  return DBSQL_OK;
}
/* Note that for this virtual table, the create and connect
** methods are identical. */

static int __fs_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}
/* The disconnect and destroy methods are also the same */

/*
** Open a new fs cursor.
*/
static int __fs_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  fs_cursor *cur;
  cur = __malloc_zero(sizeof(fs_cursor));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Close a fs cursor.
*/
static int __fs_close(dbsql_vtab_cursor_t *cur){
  fs_cursor *p_cur = (fs_cursor *)cur;
  dbsql_finalize(p_cur->stmt);
  dbsql_free(p_cur->buf);
  dbsql_free(p_cur);
  return DBSQL_OK;
}

static int __fs_next(dbsql_vtab_cursor_t *cur){
  fs_cursor *p_cur = (fs_cursor *)cur;
  int rc;

  rc = dbsql_step(p_cur->stmt);
  if( rc==DBSQL_ROW || rc==DBSQL_DONE ) rc = DBSQL_OK;

  return rc;
}

static int __fs_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  int rc;
  fs_cursor *cur = (fs_cursor *)vtab_cursor;
  fs_vtab *p = (fs_vtab *)(vtab_cursor->vtab);

  assert( (idx_num==0 && argc==0) || (idx_num==1 && argc==1) );
  if( idx_num==1 ){
    char *stmt = dbsql_mprintf(
        "SELECT * FROM %Q.%Q WHERE rowid=?", p->z_db, p->tbl);
    if( !stmt ) return DBSQL_NOMEM;
    rc = dbsql_prepare_v2(p->db, stmt, -1, &cur->stmt, 0);
    dbsql_free(stmt);
    if( rc==DBSQL_OK ){
      dbsql_bind_value(cur->stmt, 1, argv[0]);
    }
  }else{
    char *stmt = dbsql_mprintf("SELECT * FROM %Q.%Q", p->z_db, p->tbl);
    if( !stmt ) return DBSQL_NOMEM;
    rc = dbsql_prepare_v2(p->db, stmt, -1, &cur->stmt, 0);
    dbsql_free(stmt);
  }

  if( rc==DBSQL_OK ){
    rc = __fs_next(vtab_cursor); 
  }
  return rc;
}

static int __fs_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  fs_cursor *p_cur = (fs_cursor*)cur;

  assert( i==0 || i==1 || i==2 );
  if( i==0 ){
    dbsql_result_value(ctx, dbsql_column_value(p_cur->stmt, 0));
  }else{
    const char *file = (const char *)dbsql_column_text(p_cur->stmt, 1);
    struct stat sbuf;
    int fd;

    int n;
    fd = open(file, O_RDONLY);
    if( fd<0 ) return DBSQL_IOERR;
    fstat(fd, &sbuf);

    if( sbuf.st_size>=p_cur->n_alloc ){
      int64_t n_new = sbuf.st_size*2;
      char *z_new;
      if( n_new<1024 ) n_new = 1024;

      z_new = __dbsql_realloc(p_cur->buf, n_new);
      if( z_new==0 ){
        close(fd);
        return DBSQL_NOMEM;
      }
      p_cur->buf = z_new;
      p_cur->n_alloc = n_new;
    }

    n = (int)read(fd, p_cur->buf, sbuf.st_size);
    close(fd);
    if( n!=sbuf.st_size ) return DBSQL_ERROR;
    p_cur->n_buf = sbuf.st_size;
    p_cur->buf[p_cur->n_buf] = '\0';

    dbsql_result_text(ctx, p_cur->buf, -1, DBSQL_TRANSIENT);
  }
  return DBSQL_OK;
}

static int __fs_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  fs_cursor *p_cur = (fs_cursor*)cur;
  *rowid = dbsql_column_int64(p_cur->stmt, 0);
  return DBSQL_OK;
}

static int __fs_eof(dbsql_vtab_cursor_t *cur){
  fs_cursor *p_cur = (fs_cursor*)cur;
  return (dbsql_data_count(p_cur->stmt)==0);
}

static int __fs_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int ii;

  for(ii=0; ii<idx_info->n_constraint; ii++){
    struct dbsql_index_constraint_t const *cons = &idx_info->constraints[ii];
    if( cons->i_column<0 && cons->usable
           && cons->op==DBSQL_INDEX_CONSTRAINT_EQ ){
      struct dbsql_index_constraint_usage_t *usage;
      usage = &idx_info->constraint_usages[ii];
      usage->omit = 0;
      usage->argv_index = 1;
      idx_info->idx_num = 1;
      idx_info->estimated_cost = 1.0;
      break;
    }
  }

  return DBSQL_OK;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static dbsql_module_t __fs_module = {
  0,                         /* i_version */
  __fs_connect,
  __fs_connect,
  __fs_best_index,
  __fs_disconnect, 
  __fs_disconnect,
  __fs_open,                      /* x_open - open a cursor */
  __fs_close,                     /* x_close - close a cursor */
  __fs_filter,                    /* filter - configure scan constraints */
  __fs_next,                      /* next - advance a cursor */
  __fs_eof,                       /* eof - check for end of scan */
  __fs_column,                    /* column - read data */
  __fs_rowid,                     /* rowid - read data */
  0,                           /* update */
  0,                           /* begin */
  0,                           /* x_sync */
  0,                           /* commit */
  0,                           /* rollback */
  0,                           /* find_method */
  0,                           /* rename */
  0,                           /* savepoint */
  0,                           /* release */
  0,                           /* rollback_to */
  0,                           /* shadow_name */
  0                            /* integrity */
};

static dbsql_module_t __fsdir_module = {
  0,                              /* i_version */
  __fsdir_connect,                   /* create */
  __fsdir_connect,                   /* connect */
  __fsdir_best_index,                 /* best_index */
  __fsdir_disconnect,                /* disconnect */
  __fsdir_disconnect,                /* destroy */
  __fsdir_open,                      /* x_open - open a cursor */
  __fsdir_close,                     /* x_close - close a cursor */
  __fsdir_filter,                    /* filter - configure scan constraints */
  __fsdir_next,                      /* next - advance a cursor */
  __fsdir_eof,                       /* eof - check for end of scan */
  __fsdir_column,                    /* column - read data */
  __fsdir_rowid,                     /* rowid - read data */
  0,                              /* update */
  0,                              /* begin */
  0,                              /* x_sync */
  0,                              /* commit */
  0,                              /* rollback */
  0,                              /* find_method */
  0,                              /* rename */
  0,                              /* savepoint */
  0,                              /* release */
  0,                              /* rollback_to */
  0,                              /* shadow_name */
  0                               /* integrity */
};

static dbsql_module_t __fstree_module = {
  0,                              /* i_version */
  __fstree_connect,                  /* create */
  __fstree_connect,                  /* connect */
  __fstree_best_index,                /* best_index */
  __fstree_disconnect,               /* disconnect */
  __fstree_disconnect,               /* destroy */
  __fstree_open,                     /* x_open - open a cursor */
  __fstree_close,                    /* x_close - close a cursor */
  __fstree_filter,                   /* filter - configure scan constraints */
  __fstree_next,                     /* next - advance a cursor */
  __fstree_eof,                      /* eof - check for end of scan */
  __fstree_column,                   /* column - read data */
  __fstree_rowid,                    /* rowid - read data */
  0,                              /* update */
  0,                              /* begin */
  0,                              /* x_sync */
  0,                              /* commit */
  0,                              /* rollback */
  0,                              /* find_method */
  0,                              /* rename */
  0,                              /* savepoint */
  0,                              /* release */
  0,                              /* rollback_to */
  0,                              /* shadow_name */
  0                               /* integrity */
};

/*
** Decode a pointer to an DBSQL object.
*/
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);

/*
** Register the echo virtual table module.
*/
static int DBSQL_TCLAPI register_fs_module(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  DBSQL *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_create_module(db, "fs", &__fs_module, (void *)interp);
  dbsql_create_module(db, "fsdir", &__fsdir_module, 0);
  dbsql_create_module(db, "fstree", &__fstree_module, 0);
#endif
  return TCL_OK;
}

#endif


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltestfs_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "register_fs_module",   register_fs_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
