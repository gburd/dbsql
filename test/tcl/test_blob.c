/*
** 2014 October 30
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
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef DBSQL_OMIT_INCRBLOB

/* These functions are implemented in main.c. */
extern const char *__err_name(int);

/* From test1.c: */
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);
extern void *__test_text_to_ptr(const char *z);

/*
** Return a pointer to a buffer containing a text representation of the
** pointer passed as the only argument. The original pointer may be extracted
** from the text using __test_text_to_ptr().
*/
static char *__ptr_to_text(void *p){
  static char buf[100];
  dbsql_snprintf(sizeof(buf)-1, buf, "%p", p);
  return buf;
}

/*
** Attempt to extract a blob handle (type dbsql_blob_t*) from the Tcl
** object passed as the second argument. If successful, set *blob to
** point to the blob handle and return TCL_OK. Otherwise, store an error
** message in the tcl interpreter and return TCL_ERROR. The final value
** of *blob is undefined in this case.
**
** If the object contains a string that begins with "incrblob_", then it
** is assumed to be the name of a Tcl channel opened using the [db incrblob] 
** command (see tclsqlite.c). Otherwise, it is assumed to be a pointer 
** encoded using the __ptr_to_text() routine or similar.
*/
static int __blob_handle_from_obj(
  Tcl_Interp *interp, 
  Tcl_Obj *obj,
  dbsql_blob_t **blob
){
  char *z;
  TCL_SIZE n;

  z = Tcl_GetStringFromObj(obj, &n);
  if( n==0 ){
    *blob = 0;
  }else if( n>9 && 0==memcmp("incrblob_", z, 9) ){
    int not_used;
    Tcl_Channel channel;
    ClientData instanceData;
    
    channel = Tcl_GetChannel(interp, z, &not_used);
    if( !channel ) return TCL_ERROR;

    Tcl_Flush(channel);
    Tcl_Seek(channel, 0, SEEK_SET);

    instanceData = Tcl_GetChannelInstanceData(channel);
    *blob = *((dbsql_blob_t **)instanceData);
  }else{
    *blob = (dbsql_blob_t*)__test_text_to_ptr(z);
  }

  return TCL_OK;
}

/*
** Like Tcl_GetString(), except that if the string is 0 bytes in size, a
** NULL Pointer is returned.
*/
static char *__blob_string_from_obj(Tcl_Obj *obj){
  TCL_SIZE n;
  char *z;
  z = Tcl_GetStringFromObj(obj, &n);
  return (n ? z : 0);
}

/*
** dbsql_blob_open DB DATABASE TABLE COLUMN ROWID FLAGS VARNAME
**
** Tcl test harness for the dbsql_blob_open() function.
*/
static int DBSQL_TCLAPI test_blob_open(
  ClientData clientData,          /* Not used */
  Tcl_Interp *interp,             /* Calling TCL interpreter */
  int objc,                       /* Number of arguments */
  Tcl_Obj *CONST objv[]           /* Command arguments */
){
  DBSQL *db;
  const char *z_db;
  const char *table;
  const char *column;
  Tcl_WideInt i_rowid;
  int flags;
  const char *varname;
  TCL_SIZE n_varname;

  dbsql_blob_t *blob = (dbsql_blob_t*)&flags;   /* Non-zero initialization */
  int rc;

  if( objc!=8 ){
    const char *usage = "DB DATABASE TABLE COLUMN ROWID FLAGS VARNAME";
    Tcl_WrongNumArgs(interp, 1, objv, usage);
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  z_db = Tcl_GetString(objv[2]);
  table = __blob_string_from_obj(objv[3]);
  column = Tcl_GetString(objv[4]);
  if( Tcl_GetWideIntFromObj(interp, objv[5], &i_rowid) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[6], &flags) ) return TCL_ERROR;
  varname = Tcl_GetStringFromObj(objv[7], &n_varname);

  if( n_varname>0 ){
    rc = dbsql_blob_open(db, z_db, table, column, i_rowid, flags, &blob);
    Tcl_SetVar(interp, varname, __ptr_to_text(blob), 0);
  }else{
    rc = dbsql_blob_open(db, z_db, table, column, i_rowid, flags, 0);
  }

  if( rc==DBSQL_OK ){
    Tcl_ResetResult(interp);
  }else{
    Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}


/*
** dbsql_blob_close  HANDLE
*/
static int DBSQL_TCLAPI test_blob_close(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_blob_t *blob;
  int rc;
  
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }

  if( __blob_handle_from_obj(interp, objv[1], &blob) ) return TCL_ERROR;
  rc = dbsql_blob_close(blob);

  if( rc ){
    Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
  }else{
    Tcl_ResetResult(interp);
  }
  return TCL_OK;
}

/*
** dbsql_blob_bytes  HANDLE
*/
static int DBSQL_TCLAPI test_blob_bytes(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_blob_t *blob;
  int n_byte;
  
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE");
    return TCL_ERROR;
  }

  if( __blob_handle_from_obj(interp, objv[1], &blob) ) return TCL_ERROR;
  n_byte = dbsql_blob_bytes(blob);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(n_byte));

  return TCL_OK;
}

/*
** dbsql_blob_read  CHANNEL OFFSET N
**
**   This command is used to test the dbsql_blob_read() in ways that
**   the Tcl channel interface does not. The first argument should
**   be the name of a valid channel created by the [incrblob] method
**   of a database handle. This function calls dbsql_blob_read()
**   to read N bytes from offset OFFSET from the underlying dbsql
**   blob handle.
**
**   On success, a byte-array object containing the read data is 
**   returned. On failure, the interpreter result is set to the
**   text representation of the returned error code (i.e. "DBSQL_NOMEM")
**   and a Tcl exception is thrown.
*/
static int DBSQL_TCLAPI test_blob_read(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_blob_t *blob;
  int n_byte;
  int i_offset;
  unsigned char *buf = 0;
  int rc;
  
  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CHANNEL OFFSET N");
    return TCL_ERROR;
  }

  if( __blob_handle_from_obj(interp, objv[1], &blob) ) return TCL_ERROR;
  if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &i_offset)
   || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &n_byte)
  ){ 
    return TCL_ERROR;
  }

  if( n_byte>0 ){
    buf = (unsigned char *)Tcl_AttemptAlloc(n_byte);
    if( buf==0 ){
      Tcl_AppendResult(interp, "out of memory in " __FILE__, NULL);
      return TCL_ERROR;
    }
  }
  rc = dbsql_blob_read(blob, buf, n_byte, i_offset);
  if( rc==DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(buf, n_byte));
  }else{
    Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  }
  Tcl_Free((char *)buf);

  return (rc==DBSQL_OK ? TCL_OK : TCL_ERROR);
}

/*
** dbsql_blob_write HANDLE OFFSET DATA ?NDATA?
**
**   This command is used to test the dbsql_blob_write() in ways that
**   the Tcl channel interface does not. The first argument should
**   be the name of a valid channel created by the [incrblob] method
**   of a database handle. This function calls dbsql_blob_write()
**   to write the DATA byte-array to the underlying dbsql blob handle.
**   at offset OFFSET.
**
**   On success, an empty string is returned. On failure, the interpreter
**   result is set to the text representation of the returned error code 
**   (i.e. "DBSQL_NOMEM") and a Tcl exception is thrown.
*/
static int DBSQL_TCLAPI test_blob_write(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_blob_t *blob;
  int i_offset;
  int rc;

  unsigned char *buf;
  TCL_SIZE n_buf;
  int n;
  
  if( objc!=4 && objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE OFFSET DATA ?NDATA?");
    return TCL_ERROR;
  }

  if( __blob_handle_from_obj(interp, objv[1], &blob) ) return TCL_ERROR;
  if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &i_offset) ){ 
    return TCL_ERROR;
  }

  buf = Tcl_GetByteArrayFromObj(objv[3], &n_buf);
  n = (int)(n_buf & 0x7fffffff);
  if( objc==5 && Tcl_GetIntFromObj(interp, objv[4], &n) ){
    return TCL_ERROR;
  }
  rc = dbsql_blob_write(blob, buf, n, i_offset);
  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  }

  return (rc==DBSQL_OK ? TCL_OK : TCL_ERROR);
}
#endif /* DBSQL_OMIT_INCRBLOB */

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest_blob_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_INCRBLOB
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } obj_cmds[] = {
     { "dbsql_blob_open",            test_blob_open        },
     { "dbsql_blob_close",           test_blob_close       },
     { "dbsql_blob_bytes",           test_blob_bytes       },
     { "dbsql_blob_read",            test_blob_read        },
     { "dbsql_blob_write",           test_blob_write       },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, 0, 0);
  }
#endif /* DBSQL_OMIT_INCRBLOB */
  return TCL_OK;
}
