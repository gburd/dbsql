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
** Code for testing the utf.c module in dbsql.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library. Specifically, the code in this file
** is used for testing the dbsql routines for converting between
** the various supported unicode encodings.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>

/*
** The first argument is a TCL UTF-8 string. Return the byte array
** object with the encoded representation of the string, including
** the NULL terminator.
*/
static int DBSQL_TCLAPI binarize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  TCL_SIZE len;
  char *bytes;
  Tcl_Obj *ret;
  assert(objc==2);

  bytes = Tcl_GetStringFromObj(objv[1], &len);
  ret = Tcl_NewByteArrayObj((u_int8_t*)bytes, len+1);
  Tcl_SetObjResult(interp, ret);
  return TCL_OK;
}

/*
** Usage: test_value_overhead <repeat-count> <do-calls>.
**
** This routine is used to test the overhead of calls to
** dbsql_value_text(), on a value that contains a UTF-8 string. The idea
** is to figure out whether or not it is a problem to use dbsql_value_t
** structures with collation sequence functions.
**
** If <do-calls> is 0, then the calls to dbsql_value_text() are not
** actually made.
*/
static int DBSQL_TCLAPI test_value_overhead(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int do_calls;
  int repeat_count;
  int i;
  mem_t val;

  if( objc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"",
        Tcl_GetStringFromObj(objv[0], 0), " <repeat-count> <do-calls>", NULL);
    return TCL_ERROR;
  }

  if( Tcl_GetIntFromObj(interp, objv[1], &repeat_count) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &do_calls) ) return TCL_ERROR;

  val.flags = MEM_Str|MEM_Term|MEM_Static;
  val.z = "hello world";
  val.enc = DBSQL_UTF8;

  for(i=0; i<repeat_count; i++){
    if( do_calls ){
      dbsql_value_text(&val);
    }
  }

  return TCL_OK;
}

static u_int8_t name_to_enc(Tcl_Interp *interp, Tcl_Obj *obj){
  struct enc_name_t {
    char *name;
    u_int8_t enc;
  } encnames[] = {
    { "UTF8", DBSQL_UTF8 },
    { "UTF16LE", DBSQL_UTF16LE },
    { "UTF16BE", DBSQL_UTF16BE },
    { "UTF16", DBSQL_UTF16 },
    { 0, 0 }
  };
  struct enc_name_t *p_enc;
  char *z = Tcl_GetString(obj);
  for(p_enc=&encnames[0]; p_enc->name; p_enc++){
    if( 0==__str_icmp(z, p_enc->name) ){
      break;
    }
  }
  if( !p_enc->enc ){
    Tcl_AppendResult(interp, "No such encoding: ", z, NULL);
  }
  if( p_enc->enc==DBSQL_UTF16 ){
    return DBSQL_UTF16NATIVE;
  }
  return p_enc->enc;
}

/*
** Usage:   test_translate <string/blob> <from enc> <to enc> ?<transient>?
**
*/
static int DBSQL_TCLAPI test_translate(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  u_int8_t enc_from;
  u_int8_t enc_to;
  dbsql_value_t *val;

  char *z;
  TCL_SIZE len;
  void (*del)(void *p) = DBSQL_STATIC;

  if( objc!=4 && objc!=5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"",
        Tcl_GetStringFromObj(objv[0], 0), 
        " <string/blob> <from enc> <to enc>", NULL
    );
    return TCL_ERROR;
  }
  if( objc==5 ){
    del = dbsql_free;
  }

  enc_from = name_to_enc(interp, objv[2]);
  if( !enc_from ) return TCL_ERROR;
  enc_to = name_to_enc(interp, objv[3]);
  if( !enc_to ) return TCL_ERROR;

  val = __value_new(0);

  if( enc_from==DBSQL_UTF8 ){
    z = Tcl_GetString(objv[1]);
    if( objc==5 ){
      z = dbsql_mprintf("%s", z);
    }
    __value_set_str(val, -1, z, enc_from, del);
  }else{
    z = (char*)Tcl_GetByteArrayFromObj(objv[1], &len);
    if( objc==5 ){
      char *tmp = z;
      z = dbsql_malloc64(len);
      memcpy(z, tmp, len);
    }
    __value_set_str(val, -1, z, enc_from, del);
  }

  z = (char *)__value_text(val, enc_to);
  len = __value_bytes(val, enc_to) + (enc_to==DBSQL_UTF8?1:2);
  Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((u_int8_t*)z, len));

  __value_free(val);

  return TCL_OK;
}

/*
** Usage: translate_selftest
**
** Call __utf_self_test() to run the internal tests for unicode
** translation. If there is a problem an assert() will fail.
**/
void __utf_self_test(void);
static int DBSQL_TCLAPI test_translate_selftest(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifndef DBSQL_OMIT_UTF16
  __utf_self_test();
#endif
  return DBSQL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest5_Init(Tcl_Interp *interp){
  static struct {
    char *name;
    Tcl_ObjCmdProc *proc;
  } cmds[] = {
    { "binarize",                (Tcl_ObjCmdProc*)binarize },
    { "test_value_overhead",     (Tcl_ObjCmdProc*)test_value_overhead },
    { "test_translate",          (Tcl_ObjCmdProc*)test_translate     },
    { "translate_selftest",      (Tcl_ObjCmdProc*)test_translate_selftest},
  };
  int i;
  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateObjCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }
  return DBSQL_OK;
}
