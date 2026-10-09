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
** Code for testing the btree.c module in dbsql.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library.
*/
#include "dbsql_int.h"
#ifdef DBSQL_SM_LIBDB
#include "sm_int.h"
#else
#include "btree_int.h"
#endif
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>

extern const char *__err_name(int);

/*
** A bogus DBSQL connection structure for use in the btree
** tests.
*/
static DBSQL __s_db;
static int n_refdbsql = 0;

/*
** Usage:   btree_open FILENAME NCACHE
**
** Open a new database
*/
static int DBSQL_TCLAPI btree_open(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int rc, n_cache;
  char buf[100];
  int n;
  char *filename;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " FILENAME NCACHE FLAGS\"", NULL);
    return TCL_ERROR;
  }

  if( Tcl_GetInt(interp, argv[2], &n_cache) ) return TCL_ERROR;
  n_refdbsql++;
  if( n_refdbsql==1 ){
    __s_db.vfs = dbsql_vfs_find(0);
    __s_db.mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_RECURSIVE);
    dbsql_mutex_enter(__s_db.mutex);
  }
  n = (int)strlen(argv[1]);
  filename = dbsql_malloc( n+2 );
  if( filename==0 ) return TCL_ERROR;
  memcpy(filename, argv[1], n+1);
  filename[n+1] = 0;
  rc = __sm_open(__s_db.vfs, filename, &__s_db, &bt, 0, 
     DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE | DBSQL_OPEN_MAIN_DB);
  dbsql_free(filename);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  __sm_set_cache_size(bt, n_cache);
  dbsql_snprintf(sizeof(buf), buf,"%p", bt);
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage:   btree_close ID
**
** Close the given database.
*/
static int DBSQL_TCLAPI btree_close(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
  rc = __sm_close(bt);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  n_refdbsql--;
  if( n_refdbsql==0 ){
    dbsql_mutex_leave(__s_db.mutex);
    dbsql_mutex_free(__s_db.mutex);
    __s_db.mutex = 0;
    __s_db.vfs = 0;
  }
  return TCL_OK;
}


/*
** Usage:   btree_begin_transaction ID
**
** Start a new transaction
*/
static int DBSQL_TCLAPI btree_begin_transaction(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
  __sm_enter(bt);
  rc = __sm_begin_trans(bt, 1, 0);
  __sm_leave(bt);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   btree_pager_stats ID
**
** Returns pager statistics
*/
static int DBSQL_TCLAPI btree_pager_stats(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int i;
  int *a;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
 
  /* Normally in this file, with a b-tree handle opened using the 
  ** [btree_open] command it is safe to call __sm_enter() directly.
  ** But this function is sometimes called with a btree handle obtained
  ** from an open dbsql connection (using [btree_from_db]). In this case
  ** we need to obtain the mutex for the controlling dbsql handle before
  ** it is safe to call __sm_enter().
  */
  dbsql_mutex_enter(bt->db->mutex);

  __sm_enter(bt);
  a = __sm_pager_stats(__sm_pager(bt));
  for(i=0; i<11; i++){
    static char *name[] = {
      "ref", "page", "max", "size", "state", "err",
      "hit", "miss", "ovfl", "read", "write"
    };
    char buf[100];
    Tcl_AppendElement(interp, name[i]);
    dbsql_snprintf(sizeof(buf), buf,"%d",a[i]);
    Tcl_AppendElement(interp, buf);
  }
  __sm_leave(bt);

  /* Release the mutex on the dbsql handle that controls this b-tree */
  dbsql_mutex_leave(bt->db->mutex);
  return TCL_OK;
}

/*
** Usage:   btree_cursor ID TABLENUM WRITEABLE
**
** Create a new cursor.  Return the ID for the cursor.
*/
static int DBSQL_TCLAPI btree_cursor(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int i_table;
  sm_cursor_t *cur;
  int rc = DBSQL_OK;
  int wr_flag;
  char buf[30];

  if( argc!=4 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID TABLENUM WRITEABLE\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &i_table) ) return TCL_ERROR;
  if( Tcl_GetBoolean(interp, argv[3], &wr_flag) ) return TCL_ERROR;
  if( wr_flag ) wr_flag = BTREE_WRCSR;
  cur = (sm_cursor_t *)ckalloc(__sm_cursor_size());
  memset(cur, 0, __sm_cursor_size());
  dbsql_mutex_enter(bt->db->mutex);
  __sm_enter(bt);
#ifndef DBSQL_OMIT_SHARED_CACHE
  rc = __sm_lock_table(bt, i_table, !!wr_flag);
#endif
  if( rc==DBSQL_OK ){
    rc = __sm_cursor(bt, i_table, wr_flag, 0, cur);
  }
  __sm_leave(bt);
  dbsql_mutex_leave(bt->db->mutex);
  if( rc ){
    ckfree((char *)cur);
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  dbsql_snprintf(sizeof(buf), buf,"%p", cur);
  Tcl_AppendResult(interp, buf, NULL);
  return DBSQL_OK;
}

/*
** Usage:   btree_close_cursor ID
**
** Close a cursor opened using btree_cursor.
*/
static int DBSQL_TCLAPI btree_close_cursor(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_cursor_t *cur;
  int rc;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  cur = __test_text_to_ptr(argv[1]);
#if DBSQL_THREADSAFE>0
  {
    sm_t *bt = cur->btree;
    dbsql_mutex_enter(bt->db->mutex);
    __sm_enter(bt);
    rc = __sm_close_cursor(cur);
    __sm_leave(bt);
    dbsql_mutex_leave(bt->db->mutex);
  }
#else
  rc = __sm_close_cursor(cur);
#endif
  ckfree((char *)cur);
  if( rc ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return DBSQL_OK;
}

/*
** Usage:   btree_next ID
**
** Move the cursor to the next entry in the table.  Return 0 on success
** or 1 if the cursor was already on the last entry in the table or if
** the table is empty.
*/
static int DBSQL_TCLAPI btree_next(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_cursor_t *cur;
  int rc;
  int res = 0;
  char buf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  cur = __test_text_to_ptr(argv[1]);
  __sm_enter(cur->btree);
  rc = __sm_next(cur, 0);
  if( rc==DBSQL_DONE ){
    res = 1;
    rc = DBSQL_OK;
  }
  __sm_leave(cur->btree);
  if( rc ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  dbsql_snprintf(sizeof(buf),buf,"%d",res);
  Tcl_AppendResult(interp, buf, NULL);
  return DBSQL_OK;
}

/*
** Usage:   btree_first ID
**
** Move the cursor to the first entry in the table.  Return 0 if the
** cursor was left point to something and 1 if the table is empty.
*/
static int DBSQL_TCLAPI btree_first(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_cursor_t *cur;
  int rc;
  int res = 0;
  char buf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  cur = __test_text_to_ptr(argv[1]);
  __sm_enter(cur->btree);
  rc = __sm_first(cur, &res);
  __sm_leave(cur->btree);
  if( rc ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  dbsql_snprintf(sizeof(buf),buf,"%d",res);
  Tcl_AppendResult(interp, buf, NULL);
  return DBSQL_OK;
}

/*
** Usage:   btree_eof ID
**
** Return TRUE if the given cursor is not pointing at a valid entry.
** Return FALSE if the cursor does point to a valid entry.
*/
static int DBSQL_TCLAPI btree_eof(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_cursor_t *cur;
  int rc;
  char buf[50];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  cur = __test_text_to_ptr(argv[1]);
  __sm_enter(cur->btree);
  rc = __sm_eof(cur);
  __sm_leave(cur->btree);
  dbsql_snprintf(sizeof(buf),buf, "%d", rc);
  Tcl_AppendResult(interp, buf, NULL);
  return DBSQL_OK;
}

/*
** Usage:   btree_payload_size ID
**
** Return the number of bytes of payload
*/
static int DBSQL_TCLAPI btree_payload_size(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_cursor_t *cur;
  u_int32_t n;
  char buf[50];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  cur = __test_text_to_ptr(argv[1]);
  __sm_enter(cur->btree);
  n = __sm_payload_size(cur);
  __sm_leave(cur->btree);
  dbsql_snprintf(sizeof(buf),buf, "%u", n);
  Tcl_AppendResult(interp, buf, NULL);
  return DBSQL_OK;
}

/*
** usage:   varint_test  START  MULTIPLIER  COUNT  INCREMENT
**
** This command tests the __put_varint() and __get_varint()
** routines, both for accuracy and for speed.
**
** An integer is written using __put_varint() and read back with
** __get_varint() and verified to be unchanged.  This repeats COUNT
** times.  The first integer is START*MULTIPLIER.  Each iteration
** increases the integer by INCREMENT.
**
** This command returns nothing if it works.  It returns an error message
** if something goes wrong.
*/
static int DBSQL_TCLAPI btree_varint_test(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  u_int32_t start, mult, count, incr;
  u_int64_t in, out;
  int n1, n2, i, j;
  unsigned char buf[100];
  if( argc!=5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " START MULTIPLIER COUNT INCREMENT\"", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[1], (int*)&start) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[2], (int*)&mult) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[3], (int*)&count) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[4], (int*)&incr) ) return TCL_ERROR;
  in = start;
  in *= mult;
  for(i=0; i<(int)count; i++){
    char err[200];
    n1 = __put_varint(buf, in);
    if( n1>9 || n1<1 ){
      dbsql_snprintf(sizeof(err), err,
         "putVarint returned %d - should be between 1 and 9", n1);
      Tcl_AppendResult(interp, err, NULL);
      return TCL_ERROR;
    }
    n2 = __get_varint(buf, &out);
    if( n1!=n2 ){
      dbsql_snprintf(sizeof(err), err,
          "putVarint returned %d and getVarint returned %d", n1, n2);
      Tcl_AppendResult(interp, err, NULL);
      return TCL_ERROR;
    }
    if( in!=out ){
      dbsql_snprintf(sizeof(err), err,
          "Wrote 0x%016llx and got back 0x%016llx", in, out);
      Tcl_AppendResult(interp, err, NULL);
      return TCL_ERROR;
    }
    if( (in & 0xffffffff)==in ){
      u_int32_t out32;
      n2 = GET_VARINT32(buf, out32);
      out = out32;
      if( n1!=n2 ){
        dbsql_snprintf(sizeof(err), err,
          "putVarint returned %d and GetVarint32 returned %d", 
                  n1, n2);
        Tcl_AppendResult(interp, err, NULL);
        return TCL_ERROR;
      }
      if( in!=out ){
        dbsql_snprintf(sizeof(err), err,
          "Wrote 0x%016llx and got back 0x%016llx from GetVarint32",
            in, out);
        Tcl_AppendResult(interp, err, NULL);
        return TCL_ERROR;
      }
    }

    /* In order to get realistic timings, run __get_varint 19 more times.
    ** This is because __get_varint is called about 20 times more often
    ** than __put_varint.
    */
    for(j=0; j<19; j++){
      __get_varint(buf, &out);
    }
    in += incr;
  }
  return TCL_OK;
}

/*
** usage:   btree_from_db  DB-HANDLE
**
** This command returns the btree handle for the main database associated
** with the database-handle passed as the argument. Example usage:
**
** DBSQL db test.db
** set bt [btree_from_db db]
*/
static int DBSQL_TCLAPI btree_from_db(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  char buf[100];
  Tcl_CmdInfo info;
  DBSQL *db;
  sm_t *bt;
  int i_db = 0;

  if( argc!=2 && argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " DB-HANDLE ?N?\"", NULL);
    return TCL_ERROR;
  }

  if( 1!=Tcl_GetCommandInfo(interp, argv[1], &info) ){
    Tcl_AppendResult(interp, "No such db-handle: \"", argv[1], "\"", NULL);
    return TCL_ERROR;
  }
  if( argc==3 ){
    i_db = atoi(argv[2]);
  }

  db = *((DBSQL **)info.objClientData);
  assert( db );

  bt = db->dbs[i_db].bt;
  dbsql_snprintf(sizeof(buf), buf, "%p", bt);
  Tcl_SetResult(interp, buf, TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:   btree_ismemdb ID
**
** Return true if the B-Tree is currently stored entirely in memory.
*/
static int DBSQL_TCLAPI btree_ismemdb(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  sm_t *bt;
  int res;
  dbsql_file_t *file;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
  dbsql_mutex_enter(bt->db->mutex);
  __sm_enter(bt);
  file = __sm_pager_file(__sm_pager(bt));
  res = (file->methods==0);
  __sm_leave(bt);
  dbsql_mutex_leave(bt->db->mutex);
  Tcl_SetObjResult(interp, Tcl_NewBooleanObj(res));
  return DBSQL_OK;
}

/*
** usage:   btree_set_cache_size ID NCACHE
**
** Set the size of the cache used by btree $ID.
*/
static int DBSQL_TCLAPI btree_set_cache_size(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int n_cache;
  sm_t *bt;
  
  if( argc!=3 ){
    Tcl_AppendResult(
        interp, "wrong # args: should be \"", argv[0], " BT NCACHE\"", NULL);
    return TCL_ERROR;
  }
  bt = __test_text_to_ptr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &n_cache) ) return TCL_ERROR;

  dbsql_mutex_enter(bt->db->mutex);
  __sm_enter(bt);
  __sm_set_cache_size(bt, n_cache);
  __sm_leave(bt);
  dbsql_mutex_leave(bt->db->mutex);
  return TCL_OK;
}      

/*
** usage:   btree_insert CSR ?KEY? VALUE
**
** Set the size of the cache used by btree $ID.
*/
static int DBSQL_TCLAPI btree_insert(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const objv[]
){
  sm_cursor_t *cur;
  int rc;
  sm_payload_t x;
  TCL_SIZE n;

  if( objc!=4 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-intkey? CSR KEY VALUE");
    return TCL_ERROR;
  }

  memset(&x, 0, sizeof(x));
  if( objc==4 ){
    if( Tcl_GetIntFromObj(interp, objv[2], &rc) ) return TCL_ERROR;
    x.n_key = rc;
    x.data = (void*)Tcl_GetByteArrayFromObj(objv[3], &n);
    x.n_data = (int)n;
  }else{
    x.key = (void*)Tcl_GetByteArrayFromObj(objv[2], &n);
    x.n_key = (int)n;
  }
  cur = (sm_cursor_t*)__test_text_to_ptr(Tcl_GetString(objv[1]));

  dbsql_mutex_enter(cur->btree->db->mutex);
  __sm_enter(cur->btree);
  rc = __sm_insert(cur, &x, 0, 0);
  __sm_leave(cur->btree);
  dbsql_mutex_leave(cur->btree->db->mutex);

  Tcl_ResetResult(interp);
  if( rc ){
    Tcl_AppendResult(interp, __err_name(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest3_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_CmdProc *proc;
  } cmds[] = {
     { "btree_open",               (Tcl_CmdProc*)btree_open               },
     { "btree_close",              (Tcl_CmdProc*)btree_close              },
     { "btree_begin_transaction",  (Tcl_CmdProc*)btree_begin_transaction  },
     { "btree_pager_stats",        (Tcl_CmdProc*)btree_pager_stats        },
     { "btree_cursor",             (Tcl_CmdProc*)btree_cursor             },
     { "btree_close_cursor",       (Tcl_CmdProc*)btree_close_cursor       },
     { "btree_next",               (Tcl_CmdProc*)btree_next               },
     { "btree_eof",                (Tcl_CmdProc*)btree_eof                },
     { "btree_payload_size",       (Tcl_CmdProc*)btree_payload_size       },
     { "btree_first",              (Tcl_CmdProc*)btree_first              },
     { "btree_varint_test",        (Tcl_CmdProc*)btree_varint_test        },
     { "btree_from_db",            (Tcl_CmdProc*)btree_from_db            },
     { "btree_ismemdb",            (Tcl_CmdProc*)btree_ismemdb            },
     { "btree_set_cache_size",     (Tcl_CmdProc*)btree_set_cache_size     }
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }

  Tcl_CreateObjCommand(interp, "btree_insert", btree_insert, 0, 0);

  return TCL_OK;
}
