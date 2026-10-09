/*
** 2014 Dec 01
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
*/


#ifdef DBSQL_TEST
#include "tcl_dbsql.h"

#ifdef DBSQL_ENABLE_FTS5

#include "fts5.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#ifdef DBSQL_DEBUG
extern int dbsql_fts5_may_be_corrupt;
#endif
extern int __fts5_test_register_matchinfo(DBSQL*);
extern int __fts5_test_register_tok(DBSQL*, fts5_api*);

/*************************************************************************
** This is a copy of the first part of the dbsql_db structure in 
** tclsqlite.c.  We need it here so that the get_sqlite_pointer routine
** can extract the DBSQL* pointer from an existing Tcl dbsql
** connection.
*/

extern const char *__err_name(int);

struct dbsql_db {
  DBSQL *db;
};

/*
** Decode a pointer to an DBSQL object.
*/
static int __f5t_db_pointer(Tcl_Interp *interp, Tcl_Obj *obj, DBSQL **db){
  struct dbsql_db *p;
  Tcl_CmdInfo cmd_info;
  char *z = Tcl_GetString(obj);
  if( Tcl_GetCommandInfo(interp, z, &cmd_info) ){
    p = (struct dbsql_db*)cmd_info.objClientData;
    *db = p->db;
    return TCL_OK;
  }
  return TCL_ERROR;
}

/* End of code that accesses the dbsql_db struct.
**************************************************************************/

static int __f5t_result_to_error_code(const char *res){
  struct ErrorCode {
    int rc;
    const char *error;
  } errs[] = {
    { DBSQL_DONE,  "DBSQL_DONE" },
    { DBSQL_ERROR, "DBSQL_ERROR" },
    { DBSQL_OK,    "DBSQL_OK" },
    { DBSQL_OK,    "" },
  };
  int i;

  for(i=0; i<sizeof(errs)/sizeof(errs[0]); i++){
    if( 0==dbsql_stricmp(res, errs[i].error) ){
      return errs[i].rc;
    }
  }

  return DBSQL_ERROR;
}

static int DBSQL_TCLAPI __f5t_db_and_api(
  Tcl_Interp *interp, 
  Tcl_Obj *obj, 
  DBSQL **pp_db, 
  fts5_api **pp_api
){
  DBSQL *db = 0;
  int rc = __f5t_db_pointer(interp, obj, &db);
  if( rc!=TCL_OK ){
    return TCL_ERROR;
  }else{
    dbsql_stmt_t *stmt = 0;
    fts5_api *api = 0;

    rc = dbsql_prepare_v2(db, "SELECT fts5(?1)", -1, &stmt, 0);
    if( rc!=DBSQL_OK ){
      Tcl_AppendResult(interp, "error: ", dbsql_errmsg(db), (char*)0);
      return TCL_ERROR;
    }
    dbsql_bind_pointer(stmt, 1, (void*)&api, "fts5_api_ptr", 0);
    dbsql_step(stmt);

    if( dbsql_finalize(stmt)!=DBSQL_OK ){
      Tcl_AppendResult(interp, "error: ", dbsql_errmsg(db), (char*)0);
      return TCL_ERROR;
    }

    *pp_db = db;
    *pp_api = api;
  }

  return TCL_OK;
}

typedef struct f5t_function_t f5t_function_t;
struct f5t_function_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
};

typedef struct f5t_api_t f5t_api_t;
struct f5t_api_t {
  const fts5_extension_api_t *api;
  fts5_context_t *fts;
};

/*
** An object of this type is used with the set_auxdata() and get_auxdata()
** API test wrappers. The tcl interface allows a single tcl value to be 
** saved using set_auxdata(). Instead of simply storing a pointer to the
** tcl object, the code in this file wraps it in an dbsql_malloc'd 
** instance of the following struct so that if the destructor is not 
** correctly invoked it will be reported as an dbsql memory leak.
*/
typedef struct f5t_aux_data_t f5t_aux_data_t;
struct f5t_aux_data_t {
  Tcl_Obj *obj;
};

static int __x_tokenize_cb(
  void *ctx, 
  int tflags,
  const char *token, int n_token, 
  int i_start, int i_end
){
  f5t_function_t *p = (f5t_function_t*)ctx;
  Tcl_Obj *eval = Tcl_DuplicateObj(p->script);
  int rc;

  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj(token, n_token));
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewIntObj(i_start));
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewIntObj(i_end));

  rc = Tcl_EvalObjEx(p->interp, eval, 0);
  Tcl_DecrRefCount(eval);
  if( rc==TCL_OK ){
    rc = __f5t_result_to_error_code(Tcl_GetStringResult(p->interp));
  }

  return rc;
}

static int DBSQL_TCLAPI __x_f5t_api(void*, Tcl_Interp*, int, Tcl_Obj *CONST []);

static int __x_query_phrase_cb(
  const fts5_extension_api_t *api, 
  fts5_context_t *fts, 
  void *ctx
){
  f5t_function_t *p = (f5t_function_t*)ctx;
  static int64_t i_cmd = 0;
  Tcl_Obj *eval;
  int rc;

  char cmd[64];
  f5t_api_t s_api;

  s_api.api = api;
  s_api.fts = fts;
  sprintf(cmd, "f5t_2_%lld", i_cmd++);
  Tcl_CreateObjCommand(p->interp, cmd, __x_f5t_api, &s_api, 0);

  eval = Tcl_DuplicateObj(p->script);
  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj(cmd, -1));
  rc = Tcl_EvalObjEx(p->interp, eval, 0);
  Tcl_DecrRefCount(eval);
  Tcl_DeleteCommand(p->interp, cmd);

  if( rc==TCL_OK ){
    rc = __f5t_result_to_error_code(Tcl_GetStringResult(p->interp));
  }

  return rc;
}

static void __x_set_auxdata_destructor(void *p){
  f5t_aux_data_t *data = (f5t_aux_data_t*)p;
  Tcl_DecrRefCount(data->obj);
  dbsql_free(data);
}

/*
**      api sub-command...
**
** Description...
*/
static int DBSQL_TCLAPI __x_f5t_api(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct sub_t {
    const char *name;
    int n_arg;
    const char *msg;
  } subs[] = {
    { "xColumnCount",      0, "" },                   /*  0 */
    { "xRowCount",         0, "" },                   /*  1 */
    { "xColumnTotalSize",  1, "COL" },                /*  2 */
    { "xTokenize",         2, "TEXT SCRIPT" },        /*  3 */
    { "xPhraseCount",      0, "" },                   /*  4 */
    { "xPhraseSize",       1, "PHRASE" },             /*  5 */
    { "xInstCount",        0, "" },                   /*  6 */
    { "xInst",             1, "IDX" },                /*  7 */
    { "xRowid",            0, "" },                   /*  8 */
    { "xColumnText",       1, "COL" },                /*  9 */
    { "xColumnSize",       1, "COL" },                /* 10 */
    { "xQueryPhrase",      2, "PHRASE SCRIPT" },      /* 11 */
    { "xSetAuxdata",       1, "VALUE" },              /* 12 */
    { "xGetAuxdata",       1, "CLEAR" },              /* 13 */
    { "xSetAuxdataInt",    1, "INTEGER" },            /* 14 */
    { "xGetAuxdataInt",    1, "CLEAR" },              /* 15 */
    { "xPhraseForeach",    4, "IPHRASE COLVAR OFFVAR SCRIPT" }, /* 16 */
    { "xPhraseColumnForeach", 3, "IPHRASE COLVAR SCRIPT" }, /* 17 */

    { "xQueryToken",       2, "IPHRASE ITERM" },      /* 18 */
    { "xInstToken",        2, "IDX ITERM" },          /* 19 */
    { "xColumnLocale",     1, "COL" },                /* 20 */
    { 0, 0, 0}
  };

  int rc;
  int i_sub = 0;
  f5t_api_t *p = (f5t_api_t*)clientData;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND");
    return TCL_ERROR;
  }

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[1], subs, sizeof(subs[0]), "SUB-COMMAND", 0, &i_sub
  );
  if( rc!=TCL_OK ) return rc;
  if( subs[i_sub].n_arg!=objc-2 ){
    Tcl_WrongNumArgs(interp, 1, objv, subs[i_sub].msg);
    return TCL_ERROR;
  }

#define CASE(i,str) case i: assert( strcmp(subs[i].name, str)==0 );
  switch( i_sub ){
    CASE(0, "xColumnCount") {
      int n_col;
      n_col = p->api->column_count(p->fts);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewIntObj(n_col));
      }
      break;
    }
    CASE(1, "xRowCount") {
      int64_t n_row;
      rc = p->api->row_count(p->fts, &n_row);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(n_row));
      }
      break;
    }
    CASE(2, "xColumnTotalSize") {
      int i_col;
      int64_t n_size;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_col) ) return TCL_ERROR;
      rc = p->api->column_total_size(p->fts, i_col, &n_size);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(n_size));
      }
      break;
    }
    CASE(3, "xTokenize") {
      TCL_SIZE n_text;
      char *text = Tcl_GetStringFromObj(objv[2], &n_text);
      f5t_function_t ctx;
      ctx.interp = interp;
      ctx.script = objv[3];
      rc = p->api->tokenize(p->fts, text, (int)n_text, &ctx, __x_tokenize_cb);
      if( rc==DBSQL_OK ){
        Tcl_ResetResult(interp);
      }
      return rc;
    }
    CASE(4, "xPhraseCount") {
      int n_phrase;
      n_phrase = p->api->phrase_count(p->fts);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewIntObj(n_phrase));
      }
      break;
    }
    CASE(5, "xPhraseSize") {
      int i_phrase;
      int sz;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_phrase) ){
        return TCL_ERROR;
      }
      sz = p->api->phrase_size(p->fts, i_phrase);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewIntObj(sz));
      }
      break;
    }
    CASE(6, "xInstCount") {
      int n_inst;
      rc = p->api->inst_count(p->fts, &n_inst);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewIntObj(n_inst));
      }
      break;
    }
    CASE(7, "xInst") {
      int i_idx, ip, ic, io;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_idx) ){
        return TCL_ERROR;
      }
      rc = p->api->inst(p->fts, i_idx, &ip, &ic, &io);
      if( rc==DBSQL_OK ){
        Tcl_Obj *list = Tcl_NewObj();
        Tcl_ListObjAppendElement(interp, list, Tcl_NewIntObj(ip));
        Tcl_ListObjAppendElement(interp, list, Tcl_NewIntObj(ic));
        Tcl_ListObjAppendElement(interp, list, Tcl_NewIntObj(io));
        Tcl_SetObjResult(interp, list);
      }
      break;
    }
    CASE(8, "xRowid") {
      int64_t i_rowid = p->api->rowid(p->fts);
      Tcl_SetObjResult(interp, Tcl_NewWideIntObj(i_rowid));
      break;
    }
    CASE(9, "xColumnText") {
      const char *z = 0;
      int n = 0;
      int i_col;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_col) ){
        return TCL_ERROR;
      }
      rc = p->api->column_text(p->fts, i_col, &z, &n);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(z, n));
      }
      break;
    }
    CASE(10, "xColumnSize") {
      int n = 0;
      int i_col;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_col) ){
        return TCL_ERROR;
      }
      rc = p->api->column_size(p->fts, i_col, &n);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewIntObj(n));
      }
      break;
    }
    CASE(11, "xQueryPhrase") {
      int i_phrase;
      f5t_function_t ctx;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_phrase) ){
        return TCL_ERROR;
      }
      ctx.interp = interp;
      ctx.script = objv[3];
      rc = p->api->query_phrase(p->fts, i_phrase, &ctx, __x_query_phrase_cb);
      if( rc==DBSQL_OK ){
        Tcl_ResetResult(interp);
      }
      break;
    }
    CASE(12, "xSetAuxdata") {
      f5t_aux_data_t *data = (f5t_aux_data_t*)dbsql_malloc64(sizeof(f5t_aux_data_t));
      if( data==0 ){
        Tcl_AppendResult(interp, "out of memory", (char*)0);
        return TCL_ERROR;
      }
      data->obj = objv[2];
      Tcl_IncrRefCount(data->obj);
      rc = p->api->set_auxdata(p->fts, data, __x_set_auxdata_destructor);
      break;
    }
    CASE(13, "xGetAuxdata") {
      f5t_aux_data_t *data;
      int clear;
      if( Tcl_GetBooleanFromObj(interp, objv[2], &clear) ){
        return TCL_ERROR;
      }
      data = (f5t_aux_data_t*)p->api->get_auxdata(p->fts, clear);
      if( data==0 ){
        Tcl_ResetResult(interp);
      }else{
        Tcl_SetObjResult(interp, data->obj);
        if( clear ){
          __x_set_auxdata_destructor((void*)data);
        }
      }
      break;
    }

    /* These two - set_auxdata_int and get_auxdata_int - are similar to the
    ** set_auxdata and get_auxdata methods implemented above. The difference
    ** is that they may only save an integer value as auxiliary data, and
    ** do not specify a destructor function.  */
    CASE(14, "xSetAuxdataInt") {
      int i_val;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_val) ) return TCL_ERROR;
      rc = p->api->set_auxdata(p->fts, (void*)((char*)0 + i_val), 0);
      break;
    }
    CASE(15, "xGetAuxdataInt") {
      int i_val;
      int clear;
      if( Tcl_GetBooleanFromObj(interp, objv[2], &clear) ) return TCL_ERROR;
      i_val = (int)((char*)p->api->get_auxdata(p->fts, clear) - (char*)0);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(i_val));
      break;
    }

    CASE(16, "xPhraseForeach") {
      int i_phrase;
      int i_col;
      int i_off;
      const char *colvar;
      const char *offvar;
      Tcl_Obj *script = objv[5];
      fts5_phrase_iter_t iter;

      if( Tcl_GetIntFromObj(interp, objv[2], &i_phrase) ) return TCL_ERROR;
      colvar = Tcl_GetString(objv[3]);
      offvar = Tcl_GetString(objv[4]);

      rc = p->api->phrase_first(p->fts, i_phrase, &iter, &i_col, &i_off);
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp, __err_name(rc), (char*)0);
        return TCL_ERROR;
      }
      for( ;i_col>=0; p->api->phrase_next(p->fts, &iter, &i_col, &i_off) ){
        Tcl_SetVar2Ex(interp, colvar, 0, Tcl_NewIntObj(i_col), 0);
        Tcl_SetVar2Ex(interp, offvar, 0, Tcl_NewIntObj(i_off), 0);
        rc = Tcl_EvalObjEx(interp, script, 0);
        if( rc==TCL_CONTINUE ) rc = TCL_OK;
        if( rc!=TCL_OK ){
          if( rc==TCL_BREAK ) rc = TCL_OK;
          break;
        }
      }

      break;
    }

    CASE(17, "xPhraseColumnForeach") {
      int i_phrase;
      int i_col;
      const char *colvar;
      Tcl_Obj *script = objv[4];
      fts5_phrase_iter_t iter;

      if( Tcl_GetIntFromObj(interp, objv[2], &i_phrase) ) return TCL_ERROR;
      colvar = Tcl_GetString(objv[3]);

      rc = p->api->phrase_first_column(p->fts, i_phrase, &iter, &i_col);
      if( rc!=DBSQL_OK ){
        Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
        return TCL_ERROR;
      }
      for( ; i_col>=0; p->api->phrase_next_column(p->fts, &iter, &i_col)){
        Tcl_SetVar2Ex(interp, colvar, 0, Tcl_NewIntObj(i_col), 0);
        rc = Tcl_EvalObjEx(interp, script, 0);
        if( rc==TCL_CONTINUE ) rc = TCL_OK;
        if( rc!=TCL_OK ){
          if( rc==TCL_BREAK ) rc = TCL_OK;
          break;
        }
      }

      break;
    }

    CASE(18, "xQueryToken") {
      const char *term = 0;
      int n_term = 0;
      int i_phrase = 0;
      int i_term = 0;

      if( Tcl_GetIntFromObj(interp, objv[2], &i_phrase) ) return TCL_ERROR;
      if( Tcl_GetIntFromObj(interp, objv[3], &i_term) ) return TCL_ERROR;
      rc = p->api->query_token(p->fts, i_phrase, i_term, &term, &n_term);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(term, n_term));
      }

      break;
    }

    CASE(19, "xInstToken") {
      const char *term = 0;
      int n_term = 0;
      int i_idx = 0;
      int i_term = 0;

      if( Tcl_GetIntFromObj(interp, objv[2], &i_idx) ) return TCL_ERROR;
      if( Tcl_GetIntFromObj(interp, objv[3], &i_term) ) return TCL_ERROR;
      rc = p->api->inst_token(p->fts, i_idx, i_term, &term, &n_term);
      if( rc==DBSQL_OK ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(term, n_term));
      }

      break;
    }

    CASE(20, "xColumnLocale") {
      const char *z = 0;
      int n = 0;
      int i_col;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_col) ){
        return TCL_ERROR;
      }
      rc = p->api->column_locale(p->fts, i_col, &z, &n);
      if( rc==DBSQL_OK && z ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(z, n));
      }
      break;
    }

    default: 
      assert( 0 );
      break;
  }
#undef CASE

  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
    return TCL_ERROR;
  }

  return TCL_OK;
}

static void __x_f5t_function(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  f5t_function_t *p = (f5t_function_t*)api->x_user_data(fts);
  Tcl_Obj *eval;                 /* Script to evaluate */
  int i;
  int rc;

  static int64_t i_cmd = 0;
  char cmd[64];
  f5t_api_t s_api;
  s_api.api = api;
  s_api.fts = fts;

  sprintf(cmd, "f5t_%lld", i_cmd++);
  Tcl_CreateObjCommand(p->interp, cmd, __x_f5t_api, &s_api, 0);
  eval = Tcl_DuplicateObj(p->script);
  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj(cmd, -1));

  for(i=0; i<n_val; i++){
    Tcl_Obj *obj = 0;
    switch( dbsql_value_type(vals[i]) ){
      case DBSQL_TEXT:
        obj = Tcl_NewStringObj((const char*)dbsql_value_text(vals[i]), -1);
        break;
      case DBSQL_BLOB:
        obj = Tcl_NewByteArrayObj(
            dbsql_value_blob(vals[i]), dbsql_value_bytes(vals[i])
        );
        break;
      case DBSQL_INTEGER:
        obj = Tcl_NewWideIntObj(dbsql_value_int64(vals[i]));
        break;
      case DBSQL_FLOAT:
        obj = Tcl_NewDoubleObj(dbsql_value_double(vals[i]));
        break;
      default:
        obj = Tcl_NewObj();
        break;
    }
    Tcl_ListObjAppendElement(p->interp, eval, obj);
  }

  rc = Tcl_EvalObjEx(p->interp, eval, TCL_GLOBAL_ONLY);
  Tcl_DecrRefCount(eval);
  Tcl_DeleteCommand(p->interp, cmd);

  if( rc!=TCL_OK ){
    dbsql_result_error(ctx, Tcl_GetStringResult(p->interp), -1);
  }else{
    Tcl_Obj *var = Tcl_GetObjResult(p->interp);
    const char *type = (var->typePtr ? var->typePtr->name : "");
    char c = type[0];
    if( c=='b' && strcmp(type,"bytearray")==0 && var->bytes==0 ){
      /* Only return a BLOB type if the Tcl variable is a bytearray and
      ** has no string representation. */
      TCL_SIZE nn;
      unsigned char *data = Tcl_GetByteArrayFromObj(var, &nn);
      dbsql_result_blob(ctx, data, (int)nn, DBSQL_TRANSIENT);
    }else if( c=='b' && strcmp(type,"boolean")==0 ){
      int n;
      Tcl_GetIntFromObj(0, var, &n);
      dbsql_result_int(ctx, n);
    }else if( c=='d' && strcmp(type,"double")==0 ){
      double r;
      Tcl_GetDoubleFromObj(0, var, &r);
      dbsql_result_double(ctx, r);
    }else if( (c=='w' && strcmp(type,"wideInt")==0) ||
          (c=='i' && strcmp(type,"int")==0) ){
      Tcl_WideInt v;
      Tcl_GetWideIntFromObj(0, var, &v);
      dbsql_result_int64(ctx, v);
    }else{
      TCL_SIZE nn;
      unsigned char *data = (unsigned char *)Tcl_GetStringFromObj(var, &nn);
      dbsql_result_text(ctx, (char*)data, (int)nn, DBSQL_TRANSIENT);
    }
  }
}

static void __x_f5t_destroy(void *ctx){
  f5t_function_t *p = (f5t_function_t*)ctx;
  Tcl_DecrRefCount(p->script);
  ckfree((char *)p);
}

/*
**      dbsql_fts5_create_function DB NAME SCRIPT
**
** Description...
*/
static int DBSQL_TCLAPI __f5t_create_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  char *name;
  Tcl_Obj *script;
  DBSQL *db = 0;
  fts5_api *api = 0;
  f5t_function_t *ctx = 0;
  int rc;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB NAME SCRIPT");
    return TCL_ERROR;
  }
  if( __f5t_db_and_api(interp, objv[1], &db, &api) ) return TCL_ERROR;

  name = Tcl_GetString(objv[2]);
  script = objv[3];
  ctx = (f5t_function_t*)ckalloc(sizeof(f5t_function_t));
  ctx->interp = interp;
  ctx->script = script;
  Tcl_IncrRefCount(script);

  rc = api->create_function(
      api, name, (void*)ctx, __x_f5t_function, __x_f5t_destroy
  );
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "error: ", dbsql_errmsg(db), (char*)0);
    return TCL_ERROR;
  }

  return TCL_OK;
}

typedef struct f5t_tokenize_ctx_t f5t_tokenize_ctx_t;
struct f5t_tokenize_ctx_t {
  Tcl_Obj *ret;
  int subst;
  const char *input;
};

static int __x_tokenize_cb2(
  void *ctx, 
  int tflags,
  const char *token, int n_token, 
  int i_start, int i_end
){
  f5t_tokenize_ctx_t *p = (f5t_tokenize_ctx_t*)ctx;
  if( p->subst ){
    Tcl_ListObjAppendElement(0, p->ret, Tcl_NewStringObj(token, n_token));
    Tcl_ListObjAppendElement(
        0, p->ret, Tcl_NewStringObj(&p->input[i_start], i_end-i_start)
    );
  }else{
    Tcl_ListObjAppendElement(0, p->ret, Tcl_NewStringObj(token, n_token));
    Tcl_ListObjAppendElement(0, p->ret, Tcl_NewIntObj(i_start));
    Tcl_ListObjAppendElement(0, p->ret, Tcl_NewIntObj(i_end));
  }
  return DBSQL_OK;
}


/*
**      dbsql_fts5_tokenize DB TOKENIZER TEXT
**
** Description...
*/
static int DBSQL_TCLAPI __f5t_tokenize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  char *copy = 0;
  char *text = 0;
  TCL_SIZE n_text = 0;
  DBSQL *db = 0;
  fts5_api *api = 0;
  fts5_tokenizer_t *tok = 0;
  fts5_tokenizer tokenizer;
  Tcl_Obj *ret = 0;
  void *userdata;
  int rc;

  TCL_SIZE n_arg;
  const char **args;
  f5t_tokenize_ctx_t ctx;

  if( objc!=4 && objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-subst? DB NAME TEXT");
    return TCL_ERROR;
  }
  if( objc==5 ){
    char *opt = Tcl_GetString(objv[1]);
    if( strcmp("-subst", opt) ){
      Tcl_AppendResult(interp, "unrecognized option: ", opt, (char*)0);
      return TCL_ERROR;
    }
  }
  if( __f5t_db_and_api(interp, objv[objc-3], &db, &api) ) return TCL_ERROR;
  if( Tcl_SplitList(interp, Tcl_GetString(objv[objc-2]), &n_arg, &args) ){
    return TCL_ERROR;
  }
  if( n_arg==0 ){
    Tcl_AppendResult(interp, "no such tokenizer: ", (char*)0);
    Tcl_Free((void*)args);
    return TCL_ERROR;
  }
  text = Tcl_GetStringFromObj(objv[objc-1], &n_text);

  rc = api->find_tokenizer(api, args[0], &userdata, &tokenizer);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "no such tokenizer: ", args[0], (char*)0);
    return TCL_ERROR;
  }

  rc = tokenizer.create(userdata, &args[1], (int)(n_arg-1), &tok);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "error in tokenizer.xCreate()", (char*)0);
    return TCL_ERROR;
  }

  if( n_text>0 ){
    copy = dbsql_malloc64(n_text);
    if( copy==0 ){
      tokenizer.x_delete(tok);
      Tcl_AppendResult(interp, "error in dbsql_malloc()", (char*)0);
      return TCL_ERROR;
    }else{
      memcpy(copy, text, n_text);
    }
  }

  ret = Tcl_NewObj();
  Tcl_IncrRefCount(ret);
  ctx.subst = (objc==5);
  ctx.ret = ret;
  ctx.input = copy;
  rc = tokenizer.tokenize(
      tok, (void*)&ctx, FTS5_TOKENIZE_DOCUMENT, copy,(int)n_text, __x_tokenize_cb2
  );
  tokenizer.x_delete(tok);
  dbsql_free(copy);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "error in tokenizer.xTokenize()", (char*)0);
    Tcl_DecrRefCount(ret);
    return TCL_ERROR;
  }

  Tcl_Free((void*)args);
  Tcl_SetObjResult(interp, ret);
  Tcl_DecrRefCount(ret);
  return TCL_OK;
}

/*************************************************************************
** Start of tokenizer wrapper.
*/

typedef struct f5t_tokenizer_context_t f5t_tokenizer_context_t;
typedef struct f5t_tokenizer_cb_t f5t_tokenizer_cb_t;
typedef struct f5t_tokenizer_module_t f5t_tokenizer_module_t;
typedef struct f5t_tokenizer_instance_t f5t_tokenizer_instance_t;

struct f5t_tokenizer_context_t {
  void *ctx;
  int (*x_token)(void*, int, const char*, int, int, int);
  f5t_tokenizer_instance_t *inst;
};

struct f5t_tokenizer_module_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
  void *parent_ctx;
  fts5_tokenizer_v2 parent_v2;
  fts5_tokenizer parent;
  f5t_tokenizer_context_t *context;
};

/*
** locale:
**   Within a call to tokenize_v2(), locale/n_locale store the locale
**   passed to the call by fts5. This can be retrieved by a Tcl tokenize 
**   script using [dbsql_fts5_locale].
*/
struct f5t_tokenizer_instance_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
  f5t_tokenizer_module_t *module;
  fts5_tokenizer_t *parent;
  f5t_tokenizer_context_t *context;
  const char *locale;
  int n_locale;
};

static int __f5t_tokenizer_create(
  void *ctx, 
  const char **args, 
  int n_arg, 
  fts5_tokenizer_t **out
){
  fts5_tokenizer_t *parent = 0;
  f5t_tokenizer_module_t *mod = (f5t_tokenizer_module_t*)ctx;
  Tcl_Obj *eval;
  int rc = TCL_OK;
  int i;

  assert( mod->parent_v2.create==0 || mod->parent.create==0 );
  if( mod->parent_v2.create ){
    rc = mod->parent_v2.create(mod->parent_ctx, 0, 0, &parent);
  }
  if( mod->parent.create ){
    rc = mod->parent.create(mod->parent_ctx, 0, 0, &parent);
  }

  eval = Tcl_DuplicateObj(mod->script);
  Tcl_IncrRefCount(eval);
  for(i=0; rc==TCL_OK && i<n_arg; i++){
    Tcl_Obj *obj = Tcl_NewStringObj(args[i], -1);
    rc = Tcl_ListObjAppendElement(mod->interp, eval, obj);
  }

  if( rc==TCL_OK ){
    rc = Tcl_EvalObjEx(mod->interp, eval, TCL_GLOBAL_ONLY);
  }
  Tcl_DecrRefCount(eval);

  if( rc==TCL_OK ){
    f5t_tokenizer_instance_t *inst;
    inst = (f5t_tokenizer_instance_t*)ckalloc(sizeof(f5t_tokenizer_instance_t));
    memset(inst, 0, sizeof(f5t_tokenizer_instance_t));
    inst->interp = mod->interp;
    inst->script = Tcl_GetObjResult(mod->interp);
    inst->context = mod->context;
    inst->parent = parent;
    inst->module = mod;
    Tcl_IncrRefCount(inst->script);
    *out = (fts5_tokenizer_t*)inst;
  }

  return rc;
}


static void __f5t_tokenizer_delete(fts5_tokenizer_t *p){
  f5t_tokenizer_instance_t *inst = (f5t_tokenizer_instance_t*)p;
  if( inst ){
    if( inst->parent ){
      if( inst->module->parent_v2.x_delete ){
        inst->module->parent_v2.x_delete(inst->parent);
      }else{
        inst->module->parent.x_delete(inst->parent);
      }
    }
    Tcl_DecrRefCount(inst->script);
    ckfree((char *)inst);
  }
}


static int __f5t_tokenizer_really_tokenize(
  fts5_tokenizer_t *p, 
  void *ctx,
  int flags,
  const char *text, int n_text, 
  int (*token)(void*, int, const char*, int, int, int)
){
  f5t_tokenizer_instance_t *inst = (f5t_tokenizer_instance_t*)p;
  f5t_tokenizer_instance_t *old_inst = 0;
  void *old_ctx;
  int (*old_token)(void*, int, const char*, int, int, int);
  Tcl_Obj *eval;
  int rc;
  const char *z_flags;

  old_ctx = inst->context->ctx;
  old_token = inst->context->x_token;
  old_inst = inst->context->inst;

  inst->context->ctx = ctx;
  inst->context->x_token = token;
  inst->context->inst = inst;

  assert( 
      flags==FTS5_TOKENIZE_DOCUMENT
   || flags==FTS5_TOKENIZE_AUX
   || flags==FTS5_TOKENIZE_QUERY
   || flags==(FTS5_TOKENIZE_QUERY | FTS5_TOKENIZE_PREFIX)
  );
  eval = Tcl_DuplicateObj(inst->script);
  Tcl_IncrRefCount(eval);
  switch( flags ){
    case FTS5_TOKENIZE_DOCUMENT:
      z_flags = "document";
      break;
    case FTS5_TOKENIZE_AUX:
      z_flags = "aux";
      break;
    case FTS5_TOKENIZE_QUERY:
      z_flags = "query";
      break;
    case (FTS5_TOKENIZE_PREFIX | FTS5_TOKENIZE_QUERY):
      z_flags = "prefixquery";
      break;
    default:
      assert( 0 );
      z_flags = "invalid";
      break;
  }

  Tcl_ListObjAppendElement(inst->interp, eval, Tcl_NewStringObj(z_flags, -1));
  Tcl_ListObjAppendElement(inst->interp, eval, Tcl_NewStringObj(text,n_text));
  rc = Tcl_EvalObjEx(inst->interp, eval, TCL_GLOBAL_ONLY);
  Tcl_DecrRefCount(eval);

  inst->context->ctx = old_ctx;
  inst->context->x_token = old_token;
  inst->context->inst = old_inst;
  return rc;
}

typedef struct callback_ctx_t callback_ctx_t;
struct callback_ctx_t {
  fts5_tokenizer_t *p;
  void *ctx;
  int flags;
  int (*x_token)(void*, int, const char*, int, int, int);
};

static int __f5t_tokenize_callback(
  void *ctx, 
  int tflags, 
  const char *z, int n, 
  int i_start, int i_end
){
  callback_ctx_t *p = (callback_ctx_t*)ctx;
  return __f5t_tokenizer_really_tokenize(p->p, p->ctx, p->flags, z, n, p->x_token);
}

static int __f5t_tokenizer_tokenize_v2(
  fts5_tokenizer_t *p, 
  void *p_ctx,
  int flags,
  const char *text, int n_text, 
  const char *loc, int n_loc, 
  int (*token)(void*, int, const char*, int, int, int)
){
  int rc = DBSQL_OK;
  f5t_tokenizer_instance_t *inst = (f5t_tokenizer_instance_t*)p;

  inst->locale = loc;
  inst->n_locale = n_loc;

  if( inst->parent ){
    callback_ctx_t ctx;
    ctx.p = p;
    ctx.ctx = p_ctx;
    ctx.flags = flags;
    ctx.x_token = token;
    if( inst->module->parent_v2.tokenize ){
      rc = inst->module->parent_v2.tokenize(
          inst->parent, (void*)&ctx, flags, text, n_text, 
          loc, n_loc, __f5t_tokenize_callback
      );
    }else{
      rc = inst->module->parent.tokenize(
          inst->parent, (void*)&ctx, flags, text, n_text, __f5t_tokenize_callback
      );
    }
  }else{
    rc = __f5t_tokenizer_really_tokenize(p, p_ctx, flags, text, n_text, token);
  }

  inst->locale = 0;
  inst->n_locale = 0;
  return rc;
}
static int __f5t_tokenizer_tokenize(
  fts5_tokenizer_t *p, 
  void *ctx,
  int flags,
  const char *text, int n_text, 
  int (*token)(void*, int, const char*, int, int, int)
){
  return __f5t_tokenizer_tokenize_v2(p, ctx, flags, text, n_text, 0, 0, token);
}

/*
** dbsql_fts5_locale 
*/
static int DBSQL_TCLAPI __f5t_tokenizer_locale(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  f5t_tokenizer_context_t *p = (f5t_tokenizer_context_t*)clientData;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  if( p->x_token==0 ){
    Tcl_AppendResult(interp, 
        "dbsql_fts5_locale may only be used by tokenizer callback", (char*)0
    );
    return TCL_ERROR;
  }

  Tcl_SetObjResult(interp, 
      Tcl_NewStringObj(p->inst->locale, p->inst->n_locale)
  );
  return TCL_OK;
}

/*
** dbsql_fts5_token ?-colocated? TEXT START END
*/
static int DBSQL_TCLAPI __f5t_tokenizer_return(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  f5t_tokenizer_context_t *p = (f5t_tokenizer_context_t*)clientData;
  int i_start;
  int i_end;
  TCL_SIZE n_token;
  int tflags = 0;
  char *token;
  int rc;

  if( objc==5 ){
    TCL_SIZE n_arg;
    char *arg = Tcl_GetStringFromObj(objv[1], &n_arg);
    if( n_arg<=10 && n_arg>=2 && memcmp("-colocated", arg, n_arg)==0 ){
      tflags |= FTS5_TOKEN_COLOCATED;
    }else{
      goto usage;
    }
  }else if( objc!=4 ){
    goto usage;
  }

  token = Tcl_GetStringFromObj(objv[objc-3], &n_token);
  if( Tcl_GetIntFromObj(interp, objv[objc-2], &i_start) 
   || Tcl_GetIntFromObj(interp, objv[objc-1], &i_end) 
  ){
    return TCL_ERROR;
  }

  if( p->x_token==0 ){
    Tcl_AppendResult(interp, 
        "dbsql_fts5_token may only be used by tokenizer callback", (char*)0
    );
    return TCL_ERROR;
  }

  rc = p->x_token(p->ctx, tflags, token, (int)n_token, i_start, i_end);
  Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
  return rc==DBSQL_OK ? TCL_OK : TCL_ERROR;

 usage:
  Tcl_WrongNumArgs(interp, 1, objv, "?-colocated? TEXT START END");
  return TCL_ERROR;
}

static void __f5t_del_tokenizer(void *ctx){
  f5t_tokenizer_module_t *mod = (f5t_tokenizer_module_t*)ctx;
  Tcl_DecrRefCount(mod->script);
  ckfree((char *)mod);
}

/*
**      dbsql_fts5_create_tokenizer DB NAME SCRIPT
**
** Register a tokenizer named NAME implemented by script SCRIPT. When
** a tokenizer instance is created (fts5_tokenizer.create), any tokenizer
** arguments are appended to SCRIPT and the result executed.
**
** The value returned by (SCRIPT + args) is itself a tcl script. This 
** script - call it SCRIPT2 - is executed to tokenize text using the
** tokenizer instance "returned" by SCRIPT. Specifically, to tokenize
** text SCRIPT2 is invoked with a single argument appended to it - the
** text to tokenize.
**
** SCRIPT2 should invoke the [dbsql_fts5_token] command once for each
** token within the tokenized text.
*/
static int DBSQL_TCLAPI __f5t_create_tokenizer(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  f5t_tokenizer_context_t *context = (f5t_tokenizer_context_t*)clientData;
  DBSQL *db;
  fts5_api *api;
  char *name;
  Tcl_Obj *script;
  f5t_tokenizer_module_t *mod;
  int rc = DBSQL_OK;
  int v2 = 0;                    /* True to use _v2 API */
  int i_version = 2;               /* Value for _v2.i_version */
  const char *z_parent = 0;        /* Name of parent tokenizer, if any */
  int ii = 0;

  if( objc<4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?OPTIONS? DB NAME SCRIPT");
    return TCL_ERROR;
  }

  /* parser_t any options. Set stack variables v2 and parent. */
  for(ii=1; ii<objc-3; ii++){
    int i_opt = 0;
    const char *opts[] = { "-v2", "-parent", "-version", 0 };
    if( Tcl_GetIndexFromObj(interp, objv[ii], opts, "OPTION", 0, &i_opt) ){
      return TCL_ERROR;
    }
    switch( i_opt ){
      case 0: /* -v2 */ {
        v2 = 1;
        break;
      }
      case 1: /* -parent */ {
        ii++;
        if( ii==objc-3 ){
          Tcl_AppendResult(
              interp, "option requires an argument: -parent", (char*)0
          );
          return TCL_ERROR;
        }
        z_parent = Tcl_GetString(objv[ii]);
        break;
      }
      case 2: /* -version */ {
        ii++;
        if( ii==objc-3 ){
          Tcl_AppendResult(
              interp, "option requires an argument: -version", (char*)0
          );
          return TCL_ERROR;
        }
        if( Tcl_GetIntFromObj(interp, objv[ii], &i_version) ){
          return TCL_ERROR;
        }
        break;
      }
      default:
        assert( 0 );
        break;
    }
  }

  if( __f5t_db_and_api(interp, objv[objc-3], &db, &api) ){
    return TCL_ERROR;
  }
  name = Tcl_GetString(objv[objc-2]);
  script = objv[objc-1];

  mod = (f5t_tokenizer_module_t*)ckalloc(sizeof(f5t_tokenizer_module_t));
  memset(mod, 0, sizeof(f5t_tokenizer_module_t));
  mod->interp = interp;
  mod->script = script;
  Tcl_IncrRefCount(script);
  mod->context = context;
  if( z_parent ){
    if( v2 ){
      fts5_tokenizer_v2 *parent = 0;
      rc = api->find_tokenizer_v2(api, z_parent, &mod->parent_ctx, &parent);
      if( rc==DBSQL_OK ){
        memcpy(&mod->parent_v2, parent, sizeof(fts5_tokenizer_v2));
        mod->parent_v2.x_delete(0);
      }
    }else{
      rc = api->find_tokenizer(api, z_parent, &mod->parent_ctx,&mod->parent);
      if( rc==DBSQL_OK ){
        mod->parent.x_delete(0);
      }
    }
  }

  if( rc==DBSQL_OK ){
    void *mod_ctx = (void*)mod;
    if( v2==0 ){
      fts5_tokenizer t;
      t.create = __f5t_tokenizer_create;
      t.tokenize = __f5t_tokenizer_tokenize;
      t.x_delete = __f5t_tokenizer_delete;
      rc = api->create_tokenizer(api, name, mod_ctx, &t, __f5t_del_tokenizer);
    }else{
      fts5_tokenizer_v2 t2;
      memset(&t2, 0, sizeof(t2));
      t2.i_version = i_version;
      t2.create = __f5t_tokenizer_create;
      t2.tokenize = __f5t_tokenizer_tokenize_v2;
      t2.x_delete = __f5t_tokenizer_delete;
      rc = api->create_tokenizer_v2(api, name, mod_ctx, &t2,__f5t_del_tokenizer);
    }
  }

  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, (
          v2 ? "error in fts5_api.xCreateTokenizer_v2()"
          : "error in fts5_api.xCreateTokenizer()"
    ), (char*)0);
    return TCL_ERROR;
  }

  return TCL_OK;
}

static void DBSQL_TCLAPI __x_f5t_free(ClientData clientData){
  ckfree(clientData);
}

/*
**      dbsql_fts5_may_be_corrupt BOOLEAN
**
** Set or clear the global "may-be-corrupt" flag. Return the old value.
*/
static int DBSQL_TCLAPI __f5t_may_be_corrupt(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifdef DBSQL_DEBUG
  int old = dbsql_fts5_may_be_corrupt;

  if( objc!=2 && objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?BOOLEAN?");
    return TCL_ERROR;
  }
  if( objc==2 ){
    int b_new;
    if( Tcl_GetBooleanFromObj(interp, objv[1], &b_new) ) return TCL_ERROR;
    dbsql_fts5_may_be_corrupt = b_new;
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(old));
#endif
  return TCL_OK;
}


static unsigned int __f5t_fts5_hash_key(int n_slot, const char *p, int n){
  int i;
  unsigned int h = 13;
  for(i=n-1; i>=0; i--){
    h = (h << 3) ^ h ^ p[i];
  }
  return (h % n_slot);
}

static int DBSQL_TCLAPI __f5t_token_hash(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  char *z;
  TCL_SIZE n;
  unsigned int i_val;
  int n_slot;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NSLOT TOKEN");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &n_slot) ){
    return TCL_ERROR;
  }
  z = Tcl_GetStringFromObj(objv[2], &n);

  i_val = __f5t_fts5_hash_key(n_slot, z, (int)n);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(i_val));
  return TCL_OK;
}

static int DBSQL_TCLAPI __f5t_register_matchinfo(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  DBSQL *db = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __f5t_db_pointer(interp, objv[1], &db) ){
    return TCL_ERROR;
  }

  rc = __fts5_test_register_matchinfo(db);
  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}

static int DBSQL_TCLAPI __f5t_register_tok(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  DBSQL *db = 0;
  fts5_api *api = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __f5t_db_and_api(interp, objv[1], &db, &api) ){
    return TCL_ERROR;
  }

  rc = __fts5_test_register_tok(db, api);
  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char*)__err_name(rc), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}

typedef struct origin_text_ctx_t origin_text_ctx_t;
struct origin_text_ctx_t {
  DBSQL *db;
  fts5_api *api;
};

typedef struct origin_text_tokenizer_t origin_text_tokenizer_t;
struct origin_text_tokenizer_t {
  fts5_tokenizer_t *p_tok;            /* Underlying tokenizer object */
  fts5_tokenizer tokapi;          /* API implementation for tok */
};

/*
** Delete the origin_text_ctx_t object indicated by the only argument.
*/
static void __f5t_origintext_tokenizer_delete(void *ctx){
  origin_text_ctx_t *p = (origin_text_ctx_t*)ctx;
  ckfree((char*)p);
}

static int __f5t_origintext_create(
  void *ctx, 
  const char **args, 
  int n_arg, 
  fts5_tokenizer_t **out
){
  origin_text_ctx_t *p = (origin_text_ctx_t*)ctx;
  origin_text_tokenizer_t *tok = 0;
  void *tok_ctx = 0;
  int rc = DBSQL_OK;

  tok = (origin_text_tokenizer_t*)dbsql_malloc64(sizeof(origin_text_tokenizer_t));
  if( tok==0 ){
    rc = DBSQL_NOMEM;
  }else if( n_arg<1 ){
    rc = DBSQL_ERROR;
  }else{
    /* Locate the underlying tokenizer */
    rc = p->api->find_tokenizer(p->api, args[0], &tok_ctx, &tok->tokapi);
  }

  /* Create the new tokenizer instance */
  if( rc==DBSQL_OK ){
    rc = tok->tokapi.create(tok_ctx, &args[1], n_arg-1, &tok->p_tok);
  }

  if( rc!=DBSQL_OK ){
    dbsql_free(tok);
    tok = 0;
  }
  *out = (fts5_tokenizer_t*)tok;
  return rc;
}

static void __f5t_origintext_delete(fts5_tokenizer_t *tokenizer){
  origin_text_tokenizer_t *p = (origin_text_tokenizer_t*)tokenizer;
  if( p->p_tok ){
    p->tokapi.x_delete(p->p_tok);
  }
  dbsql_free(p);
}

typedef struct origin_text_cb_t origin_text_cb_t;
struct origin_text_cb_t {
  void *ctx;
  const char *text; 
  int n_text;
  int (*x_token)(void *, int, const char *, int, int, int);

  char *bufs;                     /* Buffer to use */
  int n_buf;                       /* Allocated size of bufs[] */
};

static int __x_origin_token(
  void *ctx,                     /* Copy of 2nd argument to tokenize() */
  int tflags,                     /* Mask of FTS5_TOKEN_* flags */
  const char *token,             /* Pointer to buffer containing token */
  int n_token,                     /* Size of token in bytes */
  int i_start,                     /* Byte offset of token within input text */
  int i_end                        /* Byte offset of end of token within input */
){
  origin_text_cb_t *p = (origin_text_cb_t*)ctx;
  int ret = 0;

  if( n_token==(i_end-i_start) && 0==memcmp(token, &p->text[i_start], n_token) ){
    /* Token exactly matches document text. Pass it through as is. */
    ret = p->x_token(p->ctx, tflags, token, n_token, i_start, i_end);
  }else{
    int n_req = n_token + 1 + (i_end-i_start);
    if( n_req>p->n_buf ){
      dbsql_free(p->bufs);
      p->bufs = dbsql_malloc64(n_req*2);
      if( p->bufs==0 ) return DBSQL_NOMEM;
      p->n_buf = n_req*2;
    }

    memcpy(p->bufs, token, n_token);
    p->bufs[n_token] = '\0';
    memcpy(&p->bufs[n_token+1], &p->text[i_start], i_end-i_start);
    ret = p->x_token(p->ctx, tflags, p->bufs, n_req, i_start, i_end);
  }

  return ret;
}


static int __f5t_origintext_tokenize(
  fts5_tokenizer_t *tokenizer, 
  void *ctx,
  int flags,                      /* Mask of FTS5_TOKENIZE_* flags */
  const char *text, int n_text, 
  int (*token)(void *, int, const char *, int, int, int)
){
  origin_text_tokenizer_t *p = (origin_text_tokenizer_t*)tokenizer;
  origin_text_cb_t cb;
  int ret;

  memset(&cb, 0, sizeof(cb));
  cb.ctx = ctx;
  cb.text = text;
  cb.n_text = n_text;
  cb.x_token = token;

  ret = p->tokapi.tokenize(p->p_tok,(void*)&cb,flags,text,n_text,__x_origin_token);
  dbsql_free(cb.bufs);
  return ret;
}

/*
**      dbsql_fts5_register_origintext DB
**
** Description...
*/
static int DBSQL_TCLAPI __f5t_register_origin_text(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db = 0;
  fts5_api *api = 0;
  int rc;
  fts5_tokenizer tok = {0, 0, 0};
  origin_text_ctx_t *ctx = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __f5t_db_and_api(interp, objv[1], &db, &api) ) return TCL_ERROR;

  ctx = (origin_text_ctx_t*)ckalloc(sizeof(origin_text_ctx_t));
  ctx->db = db;
  ctx->api = api;

  tok.create = __f5t_origintext_create;
  tok.x_delete = __f5t_origintext_delete;
  tok.tokenize = __f5t_origintext_tokenize;
  rc = api->create_tokenizer(
      api, "origintext", (void*)ctx, &tok, __f5t_origintext_tokenizer_delete
  );

  Tcl_ResetResult(interp);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "error: ", dbsql_errmsg(db), (void*)0);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** This function is used to DROP an fts5 table. It works even if the data
** structures fts5 stores within the database are corrupt, which sometimes
** prevents a straight "DROP TABLE" command from succeeding.
**
** The first parameter is the database handle to use for the DROP TABLE
** operation. The second is the name of the database to drop the fts5 table
** from (i.e. "main", "temp" or the name of an attached database). The
** third parameter is the name of the fts5 table to drop.
**
** DBSQL_OK is returned if the table is successfully dropped. Or, if an
** error occurs, an dbsql error code.
*/
static int dbsql_fts5_drop_corrupt_table(
  DBSQL *db,                    /* Database handle */
  const char *z_db,                /* Database name ("main", "temp" etc.) */
  const char *tab                /* Name of fts5 table to drop */
){
  int rc = DBSQL_OK;
  int def = 0;

  rc = dbsql_db_config(db, DBSQL_DBCONFIG_DEFENSIVE, -1, &def);
  if( rc==DBSQL_OK ){
    char *script = dbsql_mprintf(
        "DELETE FROM %Q.'%q_data';"
        "DELETE FROM %Q.'%q_config';"
        "INSERT INTO %Q.'%q_data' VALUES(10, X'0000000000');"
        "INSERT INTO %Q.'%q_config' VALUES('version', 4);"
        "DROP TABLE %Q.'%q';",
        z_db, tab, z_db, tab, z_db, tab, z_db, tab, z_db, tab
    );

    if( script==0 ){
      rc = DBSQL_NOMEM;
    }else{
      if( def ) dbsql_db_config(db, DBSQL_DBCONFIG_DEFENSIVE, 0, 0);
      rc = dbsql_exec(db, script, 0, 0, 0);
      if( def ) dbsql_db_config(db, DBSQL_DBCONFIG_DEFENSIVE, 1, 0);
      dbsql_free(script);
    }
  }

  return rc;
}

/*
**      dbsql_fts5_drop_corrupt_table DB DATABASE TABLE
**
** Description...
*/
static int DBSQL_TCLAPI __f5t_drop_corrupt_table(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db = 0;
  const char *z_db = 0;
  const char *tab = 0;
  int rc = DBSQL_OK;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB DATABASE TABLE");
    return TCL_ERROR;
  }
  if( __f5t_db_pointer(interp, objv[1], &db) ){
    return TCL_ERROR;
  }
  z_db = Tcl_GetString(objv[2]);
  tab = Tcl_GetString(objv[3]);

  rc = dbsql_fts5_drop_corrupt_table(db, z_db, tab);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, "error: ", dbsql_errmsg(db), (void*)0);
    return TCL_ERROR;
  }

  return TCL_OK;
}

/*
** Free a buffer returned to dbsql by the str() function.
*/
void __f5t_free(void *p){
  char *x = (char *)p;
  ckfree(&x[-8]);
}

/*
** Implementation of str().
*/
void __f5t_str_func(dbsql_func_t *ctx, int n_arg, dbsql_value_t **args){
  const char *text = 0;
  assert( n_arg==1 );

  text = (const char*)dbsql_value_text(args[0]);
  if( text ){
    int64_t n_text = strlen(text);
    char *copy = (char*)ckalloc(n_text+8);
    if( copy==0 ){
      dbsql_result_error_nomem(ctx);
    }else{
      copy += 8;
      memcpy(copy, text, n_text);
      dbsql_result_text64(ctx, copy, n_text, __f5t_free, DBSQL_UTF8);
    }
  }
}

/*
**      dbsql_fts5_register_str DB
**
** Register the str() function with database handle DB. str() interprets
** its only argument as text and returns a copy of the value in a
** non-nul-terminated buffer.
*/
static int DBSQL_TCLAPI __f5t_register_str(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __f5t_db_pointer(interp, objv[1], &db) ){
    return TCL_ERROR;
  }

  dbsql_create_function(db, "str", 1, DBSQL_UTF8, 0, __f5t_str_func, 0, 0);

  return TCL_OK;
}

/*
** Entry point.
*/
int Fts5tcl_Init(Tcl_Interp *interp){
  static struct cmd_t {
    char *name;
    Tcl_ObjCmdProc *proc;
    int tokenize_ctx;
  } cmds[] = {
    { "dbsql_fts5_create_tokenizer",   __f5t_create_tokenizer, 1 },
    { "dbsql_fts5_token",              __f5t_tokenizer_return, 1 },
    { "dbsql_fts5_locale",             __f5t_tokenizer_locale, 1 },
    { "dbsql_fts5_tokenize",           __f5t_tokenize, 0 },
    { "dbsql_fts5_create_function",    __f5t_create_function, 0 },
    { "dbsql_fts5_may_be_corrupt",     __f5t_may_be_corrupt, 0 },
    { "dbsql_fts5_token_hash",         __f5t_token_hash, 0 },
    { "dbsql_fts5_register_matchinfo", __f5t_register_matchinfo, 0 },
    { "dbsql_fts5_register_fts5tokenize", __f5t_register_tok, 0 },
    { "dbsql_fts5_register_origintext",__f5t_register_origin_text, 0 },
    { "dbsql_fts5_drop_corrupt_table", __f5t_drop_corrupt_table, 0 },
    { "dbsql_fts5_register_str",       __f5t_register_str, 0 }
  };
  int i;
  f5t_tokenizer_context_t *context;

  context = (f5t_tokenizer_context_t*)ckalloc(sizeof(f5t_tokenizer_context_t));
  memset(context, 0, sizeof(*context));

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    struct cmd_t *p = &cmds[i];
    void *ctx = 0;
    if( p->tokenize_ctx ) ctx = (void*)context;
    Tcl_CreateObjCommand(interp, p->name, p->proc, ctx, (i ? 0 : __x_f5t_free));
  }

  return TCL_OK;
}
#else  /* DBSQL_ENABLE_FTS5 */
int Fts5tcl_Init(Tcl_Interp *interp){
  return TCL_OK;
}
#endif /* DBSQL_ENABLE_FTS5 */
#endif /* DBSQL_TEST */
