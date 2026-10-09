/*
** 2018 June 17
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

#include "dbsql.h"

#ifdef DBSQL_TEST

#include "dbsql_int.h"
#include "tcl_dbsql.h"

extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);
extern const char *__err_name(int);

typedef struct test_window_t test_window_t;
struct test_window_t {
  Tcl_Obj *x_step;
  Tcl_Obj *final;
  Tcl_Obj *value;
  Tcl_Obj *inverse;
  Tcl_Interp *interp;
};

typedef struct test_window_ctx_t test_window_ctx_t;
struct test_window_ctx_t {
  Tcl_Obj *val;
};

static void __do_test_window_step(
  int inverse,
  dbsql_func_t *ctx, 
  int n_arg, 
  dbsql_value_t **args
){
  int i;
  test_window_t *p = (test_window_t*)dbsql_user_data(ctx);
  Tcl_Obj *eval = Tcl_DuplicateObj(inverse ? p->inverse : p->x_step);
  test_window_ctx_t *p_ctx = dbsql_aggregate_context(ctx, sizeof(test_window_ctx_t));

  Tcl_IncrRefCount(eval);
  if( p_ctx ){
    const char *result;
    int rc;
    if( p_ctx->val ){
      Tcl_ListObjAppendElement(p->interp, eval, Tcl_DuplicateObj(p_ctx->val));
    }else{
      Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj("", -1));
    }
    for(i=0; i<n_arg; i++){
      Tcl_Obj *arg;
      arg = Tcl_NewStringObj((const char*)dbsql_value_text(args[i]), -1);
      Tcl_ListObjAppendElement(p->interp, eval, arg);
    }
    rc = Tcl_EvalObjEx(p->interp, eval, TCL_EVAL_GLOBAL);
    if( rc!=TCL_OK ){
      result = Tcl_GetStringResult(p->interp);
      dbsql_result_error(ctx, result, -1);
    }else{
      if( p_ctx->val ) Tcl_DecrRefCount(p_ctx->val);
      p_ctx->val = Tcl_DuplicateObj(Tcl_GetObjResult(p->interp));
      Tcl_IncrRefCount(p_ctx->val);
    }
  }
  Tcl_DecrRefCount(eval);
}

static void __do_test_window_finalize(int value, dbsql_func_t *ctx){
  test_window_t *p = (test_window_t*)dbsql_user_data(ctx);
  Tcl_Obj *eval = Tcl_DuplicateObj(value ? p->value : p->final);
  test_window_ctx_t *p_ctx = dbsql_aggregate_context(ctx, sizeof(test_window_ctx_t));

  Tcl_IncrRefCount(eval);
  if( p_ctx ){
    const char *result;
    int rc;
    if( p_ctx->val ){
      Tcl_ListObjAppendElement(p->interp, eval, Tcl_DuplicateObj(p_ctx->val));
    }else{
      Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewStringObj("", -1));
    }

    rc = Tcl_EvalObjEx(p->interp, eval, TCL_EVAL_GLOBAL);
    result = Tcl_GetStringResult(p->interp);
    if( rc!=TCL_OK ){
      dbsql_result_error(ctx, result, -1);
    }else{
      dbsql_result_text(ctx, result, -1, DBSQL_TRANSIENT);
    }

    if( value==0 ){
      if( p_ctx->val ) Tcl_DecrRefCount(p_ctx->val);
      p_ctx->val = 0;
    }
  }
  Tcl_DecrRefCount(eval);
}

static void __test_window_step(
  dbsql_func_t *ctx, 
  int n_arg, 
  dbsql_value_t **args
){
  __do_test_window_step(0, ctx, n_arg, args);
}
static void __test_window_inverse(
  dbsql_func_t *ctx, 
  int n_arg, 
  dbsql_value_t **args
){
  __do_test_window_step(1, ctx, n_arg, args);
}

static void __test_window_final(dbsql_func_t *ctx){
  __do_test_window_finalize(0, ctx);
}
static void __test_window_value(dbsql_func_t *ctx){
  __do_test_window_finalize(1, ctx);
}

static void __test_window_destroy(void *ctx){
  ckfree(ctx);
}

/*
** Usage: dbsql_create_window_function DB NAME XSTEP XFINAL XVALUE XINVERSE
*/
static int DBSQL_TCLAPI test_create_window(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  test_window_t *p_new;
  DBSQL *db;
  const char *name;
  int rc;

  if( objc!=7 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB NAME XSTEP XFINAL XVALUE XINVERSE");
    return TCL_ERROR;
  }

  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  name = Tcl_GetString(objv[2]);
  p_new = (test_window_t*)ckalloc(sizeof(test_window_t));
  memset(p_new, 0, sizeof(test_window_t));
  p_new->x_step = Tcl_DuplicateObj(objv[3]);
  p_new->final = Tcl_DuplicateObj(objv[4]);
  p_new->value = Tcl_DuplicateObj(objv[5]);
  p_new->inverse = Tcl_DuplicateObj(objv[6]);
  p_new->interp = interp;

  Tcl_IncrRefCount(p_new->x_step);
  Tcl_IncrRefCount(p_new->final);
  Tcl_IncrRefCount(p_new->value);
  Tcl_IncrRefCount(p_new->inverse);

  rc = dbsql_create_window_function(db, name, -1, DBSQL_UTF8, (void*)p_new,
      __test_window_step, __test_window_final, __test_window_value, __test_window_inverse,
      __test_window_destroy
  );
  if( rc!=DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    return TCL_ERROR;
  }

  return TCL_OK;
}

static int DBSQL_TCLAPI test_create_window_misuse(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = dbsql_create_window_function(db, "fff", -1, DBSQL_UTF8, 0,
      0, __test_window_final, __test_window_value, __test_window_inverse,
      0
  );
  if( rc!=DBSQL_MISUSE ) goto error;
  rc = dbsql_create_window_function(db, "fff", -1, DBSQL_UTF8, 0,
      __test_window_step, 0, __test_window_value, __test_window_inverse,
      0
  );
  if( rc!=DBSQL_MISUSE ) goto error;
  rc = dbsql_create_window_function(db, "fff", -1, DBSQL_UTF8, 0,
      __test_window_step, __test_window_final, 0, __test_window_inverse,
      0
  );
  if( rc!=DBSQL_MISUSE ) goto error;
  rc = dbsql_create_window_function(db, "fff", -1, DBSQL_UTF8, 0,
      __test_window_step, __test_window_final, __test_window_value, 0,
      0
  );
  if( rc!=DBSQL_MISUSE ) goto error;

  return TCL_OK;

 error:
  Tcl_SetObjResult(interp, Tcl_NewStringObj("misuse test error", -1));
  return TCL_ERROR;
}

/*
** step for sumint().
*/
static void __sumint_step(
  dbsql_func_t *ctx, 
  int n_arg, 
  dbsql_value_t *args[]
){
  int64_t *p_int;

  assert( n_arg==1 );
  if( dbsql_value_type(args[0])!=DBSQL_INTEGER ){
    dbsql_result_error(ctx, "invalid argument", -1);
    return;
  }
  p_int = (int64_t*)dbsql_aggregate_context(ctx, sizeof(int64_t));
  if( p_int ){
    *p_int += dbsql_value_int64(args[0]);
  }
}

/*
** inverse for sumint().
*/
static void __sumint_inverse(
  dbsql_func_t *ctx, 
  int n_arg, 
  dbsql_value_t *args[]
){
  int64_t *p_int;
  p_int = (int64_t*)dbsql_aggregate_context(ctx, sizeof(int64_t));
  *p_int -= dbsql_value_int64(args[0]);
}

/*
** final for sumint().
*/
static void __sumint_final(dbsql_func_t *ctx){
  int64_t res = 0;
  int64_t *p_int;
  p_int = (int64_t*)dbsql_aggregate_context(ctx, 0);
  if( p_int ) res = *p_int;
  dbsql_result_int64(ctx, res);
}

/*
** value for sumint().
*/
static void __sumint_value(dbsql_func_t *ctx){
  int64_t res = 0;
  int64_t *p_int;
  p_int = (int64_t*)dbsql_aggregate_context(ctx, 0);
  if( p_int ) res = *p_int;
  dbsql_result_int64(ctx, res);
}

static int DBSQL_TCLAPI test_create_sumint(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = dbsql_create_window_function(db, "sumint", 1, DBSQL_UTF8, 0,
      __sumint_step, __sumint_final, __sumint_value, __sumint_inverse,
      0
  );

  if( rc!=DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    return TCL_ERROR;
  }
  return TCL_OK;
}

static int DBSQL_TCLAPI test_override_sum(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = dbsql_create_function(db, "sum", -1, DBSQL_UTF8, 0,
      0, __sumint_step, __sumint_final
  );

  if( rc!=DBSQL_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    return TCL_ERROR;
  }
  return TCL_OK;
}

int Dbsqltest_window_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     int clientData;
  } obj_cmds[] = {
     { "dbsql_create_window_function", test_create_window, 0 },
     { "test_create_window_function_misuse", test_create_window_misuse, 0 },
     { "test_create_sumint", test_create_sumint, 0 },
     { "test_override_sum", test_override_sum, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    ClientData c = (ClientData)DBSQL_INT_TO_PTR(obj_cmds[i].clientData);
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, c, 0);
  }
  return TCL_OK;
}
#endif
