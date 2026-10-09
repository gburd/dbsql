/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the pager.c module in dbsql.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern const char *__err_name(int);

/*
** Page size and reserved size used for testing.
*/
static int test_pagesize = 1024;

/*
** Dummy page reinitializer
*/
static void pager_test_reiniter(db_page_t *not_used){
  return;
}

#ifndef DBSQL_SM_LIBDB
/*
** Usage:   pager_open FILENAME N-PAGE
**
** Open a new pager
*/
static int DBSQL_TCLAPI pager_open(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  u_int32_t page_size;
  pager_t *pager;
  int n_page;
  int rc;
  char buf[100];
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " FILENAME N-PAGE\"", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n_page) ) return TCL_ERROR;
  rc = __sm_pager_open(dbsql_vfs_find(0), &pager, argv[1], 0, 0,
      DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE | DBSQL_OPEN_MAIN_DB,
      pager_test_reiniter);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  __sm_pager_set_cachesize(pager, n_page);
  page_size = test_pagesize;
  __sm_pager_set_pagesize(pager, &page_size, -1);
  dbsql_snprintf(sizeof(buf),buf,"%p",pager);
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   pager_close ID
**
** Close the given pager.
*/
static int DBSQL_TCLAPI pager_close(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_close(pager, 0);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_rollback ID
**
** Rollback changes
*/
static int DBSQL_TCLAPI pager_rollback(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_rollback(pager);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_commit ID
**
** Commit all changes
*/
static int DBSQL_TCLAPI pager_commit(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_commit_phase_one(pager, 0, 0);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  rc = __sm_pager_commit_phase_two(pager);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_stmt_begin ID
**
** Start a new checkpoint.
*/
static int DBSQL_TCLAPI pager_stmt_begin(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_open_savepoint(pager, 1);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_stmt_rollback ID
**
** Rollback changes to a checkpoint
*/
static int DBSQL_TCLAPI pager_stmt_rollback(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_savepoint(pager, SAVEPOINT_ROLLBACK, 0);
  __sm_pager_savepoint(pager, SAVEPOINT_RELEASE, 0);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_stmt_commit ID
**
** Commit changes to a checkpoint
*/
static int DBSQL_TCLAPI pager_stmt_commit(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  rc = __sm_pager_savepoint(pager, SAVEPOINT_RELEASE, 0);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   pager_stats ID
**
** Return pager statistics.
*/
static int DBSQL_TCLAPI pager_stats(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int i, *a;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  a = __sm_pager_stats(pager);
  for(i=0; i<9; i++){
    static char *name[] = {
      "ref", "page", "max", "size", "state", "err",
      "hit", "miss", "ovfl",
    };
    char buf[100];
    Tcl_AppendElement(interp, name[i]);
    dbsql_snprintf(sizeof(buf),buf,"%d",a[i]);
    Tcl_AppendElement(interp, buf);
  }
  return TCL_OK;
}

/*
** Usage:   pager_pagecount ID
**
** Return the size of the database file.
*/
static int DBSQL_TCLAPI pager_pagecount(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  char buf[100];
  int n_page;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  __sm_pager_pagecount(pager, &n_page);
  dbsql_snprintf(sizeof(buf), buf, "%d", n_page);
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   page_get ID PGNO
**
** Return a pointer to a page from the database.
*/
static int DBSQL_TCLAPI page_get(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  char buf[100];
  db_page_t *page = 0;
  int pgno;
  int rc;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID PGNO\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &pgno) ) return TCL_ERROR;
  rc = __sm_pager_shared_lock(pager);
  if( rc==DBSQL_OK ){
    rc = __sm_pager_get(pager, pgno, &page, 0);
  }
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  dbsql_snprintf(sizeof(buf),buf,"%p",page);
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   page_lookup ID PGNO
**
** Return a pointer to a page if the page is already in cache.
** If not in cache, return an empty string.
*/
static int DBSQL_TCLAPI page_lookup(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  char buf[100];
  db_page_t *page;
  int pgno;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID PGNO\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &pgno) ) return TCL_ERROR;
  page = __sm_pager_lookup(pager, pgno);
  if( page ){
    dbsql_snprintf(sizeof(buf),buf,"%p",page);
    Tcl_AppendResult(interp, buf, NULL);
  }
  return TCL_OK;
}

/*
** Usage:   pager_truncate ID PGNO
*/
static int DBSQL_TCLAPI pager_truncate(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  pager_t *pager;
  int pgno;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID PGNO\"", NULL);
    return TCL_ERROR;
  }
  pager = __test_text_to_ptr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &pgno) ) return TCL_ERROR;
  __sm_pager_truncate_image(pager, pgno);
  return TCL_OK;
}


/*
** Usage:   page_unref PAGE
**
** Drop a pointer to a page.
*/
static int DBSQL_TCLAPI page_unref(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  db_page_t *page;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " PAGE\"", NULL);
    return TCL_ERROR;
  }
  page = (db_page_t *)__test_text_to_ptr(argv[1]);
  __sm_pager_unref(page);
  return TCL_OK;
}

/*
** Usage:   page_read PAGE
**
** Return the content of a page
*/
static int DBSQL_TCLAPI page_read(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  char buf[100];
  db_page_t *page;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " PAGE\"", NULL);
    return TCL_ERROR;
  }
  page = __test_text_to_ptr(argv[1]);
  memcpy(buf, __sm_pager_get_data(page), sizeof(buf));
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   page_number PAGE
**
** Return the page number for a page.
*/
static int DBSQL_TCLAPI page_number(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  char buf[100];
  db_page_t *page;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " PAGE\"", NULL);
    return TCL_ERROR;
  }
  page = (db_page_t *)__test_text_to_ptr(argv[1]);
  dbsql_snprintf(sizeof(buf), buf, "%d", __sm_pager_pagenumber(page));
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   page_write PAGE DATA
**
** Write something into a page.
*/
static int DBSQL_TCLAPI page_write(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  db_page_t *page;
  char *data;
  int rc;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " PAGE DATA\"", NULL);
    return TCL_ERROR;
  }
  page = (db_page_t *)__test_text_to_ptr(argv[1]);
  rc = __sm_pager_write(page);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  data = __sm_pager_get_data(page);
  strncpy(data, argv[2], test_pagesize-1);
  data[test_pagesize-1] = 0;
  return TCL_OK;
}

#endif /* !DBSQL_SM_LIBDB */

#ifndef DBSQL_OMIT_DISKIO
/*
** Usage:   fake_big_file  N  FILENAME
**
** Write a few bytes at the N megabyte point of FILENAME.  This will
** create a large file.  If the file was a valid dbsql database, then
** the next time the database is opened, dbsql will begin allocating
** new pages after N.  If N is 2096 or bigger, this will test the
** ability of dbsql to write to large files.
*/
static int DBSQL_TCLAPI fake_big_file(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  dbsql_vfs_t *vfs;
  dbsql_file_t *fd = 0;
  int rc;
  int n;
  int64_t offset;
  char *file;
  int n_file;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " N-MEGABYTES FILE\"", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[1], &n) ) return TCL_ERROR;
#if defined(_WIN32)
  if( n>2 ){
    Tcl_AppendResult(interp, "cannot create ", argv[1],
       "MB file because Windows "
       "does not support sparse files", (void*)0);
    return TCL_ERROR;
  }
#endif

  vfs = dbsql_vfs_find(0);
  n_file = (int)strlen(argv[2]);
  file = dbsql_malloc( n_file+2 );
  if( file==0 ) return TCL_ERROR;
  memcpy(file, argv[2], n_file+1);
  file[n_file+1] = 0;
  rc = __dbsql_os_open_malloc(vfs, file, &fd, 
      (DBSQL_OPEN_CREATE|DBSQL_OPEN_READWRITE|DBSQL_OPEN_MAIN_DB), 0
  );
  if( rc ){
    Tcl_AppendResult(interp, "open failed: ", __err_name(rc), NULL);
    dbsql_free(file);
    return TCL_ERROR;
  }
  offset = n;
  offset *= 1024*1024;
  rc = __dbsql_os_write(fd, "Hello, World!", 14, offset);
  __dbsql_os_close_free(fd);
  dbsql_free(file);
  if( rc ){
    Tcl_AppendResult(interp, "write failed: ", __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}
#endif


/*
** test_control_pending_byte  PENDING_BYTE
**
** Set the PENDING_BYTE using the dbsql_test_control() interface.
*/
static int DBSQL_TCLAPI __test_pending_byte(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int pbyte;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
                     " PENDING-BYTE\"", (void*)0);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[1], &pbyte) ) return TCL_ERROR;
  rc = dbsql_test_control(DBSQL_TESTCTRL_PENDING_BYTE, pbyte);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** The __fault_sim() callback:
*/
static Tcl_Interp *__fault_sim_interp = 0;
static int __fault_sim_script_size = 0;
static char *__fault_sim_script;
static int __fault_sim_callback(int x){
  char z_int[30];
  int i;
  int is_neg;
  int rc;
  if( x==0 ){
    memcpy(__fault_sim_script+__fault_sim_script_size, "0", 2);
  }else{
    /* Convert x to text without using any DBSQL routines */
    if( x<0 ){
      is_neg = 1;
      x = -x;
    }else{
      is_neg = 0;
    }
    z_int[sizeof(z_int)-1] = 0;
    for(i=sizeof(z_int)-2; i>0 && x>0; i--, x /= 10){
      z_int[i] = (x%10) + '0';
    }
    if( is_neg ) z_int[i--] = '-';
    memcpy(__fault_sim_script+__fault_sim_script_size, z_int+i+1, sizeof(z_int)-i-1);
  }
  rc = Tcl_Eval(__fault_sim_interp, __fault_sim_script);
  if( rc ){
    fprintf(stderr, "fault simulator script failed: [%s]", __fault_sim_script);
    rc = DBSQL_ERROR;
  }else{
    rc = atoi(Tcl_GetStringResult(__fault_sim_interp));
  }
  Tcl_ResetResult(__fault_sim_interp);
  return rc;
}

/*
** dbsql_test_control_fault_install SCRIPT
**
** Arrange to invoke SCRIPT with the integer argument to __fault_sim()
** appended, whenever __fault_sim() is called.  Or, if SCRIPT is the
** empty string, cancel the __fault_sim() callback.
*/
static int DBSQL_TCLAPI __fault_install_cmd(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  const char *script;
  int n_script;
  int rc;
  if( argc!=1 && argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
                     " SCRIPT\"", (void*)0);
    return DBSQL_ERROR;
  }
  script = argc==2 ? argv[1] : "";
  n_script = (int)strlen(script);
  if( __fault_sim_script ){
    free(__fault_sim_script);
    __fault_sim_script = 0;
  }
  if( n_script==0 ){
    rc = dbsql_test_control(DBSQL_TESTCTRL_FAULT_INSTALL, 0);
  }else{
    __fault_sim_script = malloc( n_script+100 );
    if( __fault_sim_script==0 ){
      Tcl_AppendResult(interp, "out of memory", (void*)0);
      return DBSQL_ERROR;
    }
    memcpy(__fault_sim_script, script, n_script);
    __fault_sim_script[n_script] = ' ';
    __fault_sim_script_size = n_script+1;
    __fault_sim_interp = interp;
    rc = dbsql_test_control(DBSQL_TESTCTRL_FAULT_INSTALL, __fault_sim_callback);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** __bitvec_builtin_test SIZE PROGRAM
**
** Invoke the DBSQL_TESTCTRL_BITVEC_TEST operator on test_control.
** See comments on __bitvec_builtin_test() for additional information.
*/
static int DBSQL_TCLAPI __test_bitvec_builtin_test(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int sz, rc;
  int n_prog = 0;
  int progs[100];
  const char *z;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
                     " SIZE PROGRAM\"", (void*)0);
  }
  if( Tcl_GetInt(interp, argv[1], &sz) ) return TCL_ERROR;
  z = argv[2];
  while( n_prog<99 && *z ){
    while( *z && !__isdigit(*z) ){ z++; }
    if( *z==0 ) break;
    progs[n_prog++] = atoi(z);
    while( __isdigit(*z) ){ z++; }
  }
  progs[n_prog] = 0;
  rc = dbsql_test_control(DBSQL_TESTCTRL_BITVEC_TEST, sz, progs);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}  

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest2_Init(Tcl_Interp *interp){
  extern int dbsql_io_error_persist;
  extern int dbsql_io_error_pending;
  extern int dbsql_io_error_hit;
  extern int dbsql_io_error_hardhit;
  extern int dbsql_diskfull_pending;
  extern int dbsql_diskfull;
  static struct {
    char *name;
    Tcl_CmdProc *proc;
  } cmds[] = {
#ifndef DBSQL_SM_LIBDB
    { "pager_open",              (Tcl_CmdProc*)pager_open          },
    { "pager_close",             (Tcl_CmdProc*)pager_close         },
    { "pager_commit",            (Tcl_CmdProc*)pager_commit        },
    { "pager_rollback",          (Tcl_CmdProc*)pager_rollback      },
    { "pager_stmt_begin",        (Tcl_CmdProc*)pager_stmt_begin    },
    { "pager_stmt_commit",       (Tcl_CmdProc*)pager_stmt_commit   },
    { "pager_stmt_rollback",     (Tcl_CmdProc*)pager_stmt_rollback },
    { "pager_stats",             (Tcl_CmdProc*)pager_stats         },
    { "pager_pagecount",         (Tcl_CmdProc*)pager_pagecount     },
    { "page_get",                (Tcl_CmdProc*)page_get            },
    { "page_lookup",             (Tcl_CmdProc*)page_lookup         },
    { "page_unref",              (Tcl_CmdProc*)page_unref          },
    { "page_read",               (Tcl_CmdProc*)page_read           },
    { "page_write",              (Tcl_CmdProc*)page_write          },
    { "page_number",             (Tcl_CmdProc*)page_number         },
    { "pager_truncate",          (Tcl_CmdProc*)pager_truncate      },
#endif
#ifndef DBSQL_OMIT_DISKIO
    { "fake_big_file",           (Tcl_CmdProc*)fake_big_file       },
#endif
    { "__bitvec_builtin_test",(Tcl_CmdProc*)__test_bitvec_builtin_test     },
    { "dbsql_test_control_pending_byte",  (Tcl_CmdProc*)__test_pending_byte },
    { "dbsql_test_control_fault_install", (Tcl_CmdProc*)__fault_install_cmd },
  };
  int i;
  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }
  Tcl_LinkVar(interp, "dbsql_io_error_pending",
     (char*)&dbsql_io_error_pending, TCL_LINK_INT);
  Tcl_LinkVar(interp, "dbsql_io_error_persist",
     (char*)&dbsql_io_error_persist, TCL_LINK_INT);
  Tcl_LinkVar(interp, "dbsql_io_error_hit",
     (char*)&dbsql_io_error_hit, TCL_LINK_INT);
  Tcl_LinkVar(interp, "dbsql_io_error_hardhit",
     (char*)&dbsql_io_error_hardhit, TCL_LINK_INT);
  Tcl_LinkVar(interp, "dbsql_diskfull_pending",
     (char*)&dbsql_diskfull_pending, TCL_LINK_INT);
  Tcl_LinkVar(interp, "dbsql_diskfull",
     (char*)&dbsql_diskfull, TCL_LINK_INT);
#ifndef DBSQL_OMIT_WSD
  Tcl_LinkVar(interp, "dbsql_pending_byte",
     (char*)&__pending_byte, TCL_LINK_INT | TCL_LINK_READ_ONLY);
#endif
  return TCL_OK;
}
