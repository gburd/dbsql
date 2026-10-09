/*
** 2010 August 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of dbsql interfaces. This code
** is not included in the dbsql library. 
*/

#include "dbsql.h"
#include "tcl_dbsql.h"

/* Solely for the UNUSED_PARAMETER() macro. */
#include "dbsql_int.h"

#ifdef DBSQL_ENABLE_RTREE

typedef struct box_geom_ctx_t box_geom_ctx_t;
struct box_geom_ctx_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
};

typedef struct box_query_ctx_t box_query_ctx_t;
struct box_query_ctx_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
};

static void __test_del_user(void *ctx){
  box_geom_ctx_t *p = (box_geom_ctx_t*)ctx;
  Tcl_EvalObjEx(p->interp, p->script, 0);
  Tcl_DecrRefCount(p->script);
  dbsql_free(p);
}

static int __invoke_tcl_geom_cb(
  const char *name, 
  dbsql_rtree_geometry_t *p, 
  int n_coord,
  dbsql_rtree_dbl_t *coords
){
  int rc = DBSQL_OK;
  if( p->context ){
    char ptrs[64];
    box_geom_ctx_t *ctx = (box_geom_ctx_t*)p->context;
    Tcl_Interp *interp = ctx->interp;
    Tcl_Obj *script = 0;
    Tcl_Obj *param = 0;
    Tcl_Obj *coord = 0;
    int ii;
    Tcl_Obj *res;


    script = Tcl_DuplicateObj(ctx->script);
    Tcl_IncrRefCount(script);
    Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(name,-1));

    dbsql_snprintf(sizeof(ptrs)-1, ptrs, "%p", (void*)p->context);
    Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(ptrs,-1));

    param = Tcl_NewObj();
    for(ii=0; ii<p->n_param; ii++){
      Tcl_ListObjAppendElement(
          interp, param, Tcl_NewDoubleObj(p->params[ii])
      );
    }
    Tcl_ListObjAppendElement(interp, script, param);

    coord = Tcl_NewObj();
    for(ii=0; ii<n_coord; ii++){
      Tcl_ListObjAppendElement(interp, coord, Tcl_NewDoubleObj(coords[ii]));
    }
    Tcl_ListObjAppendElement(interp, script, coord);

    dbsql_snprintf(sizeof(ptrs)-1, ptrs, "%p", (void*)p);
    Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(ptrs,-1));

    rc = Tcl_EvalObjEx(interp, script, 0);
    if( rc!=TCL_OK ){
      rc = DBSQL_ERROR;
    }else{
      TCL_SIZE n_obj = 0;
      Tcl_Obj **objs = 0;

      res = Tcl_GetObjResult(interp);
      if( Tcl_ListObjGetElements(interp, res, &n_obj, &objs) ) return TCL_ERROR;
      if( n_obj>0 ){
        const char *cmd = Tcl_GetString(objs[0]);
        if( 0==dbsql_stricmp(cmd, "zero") ){
          p->params[0] = 0.0;
          p->n_param = 1;
        }
        else if( 0==dbsql_stricmp(cmd, "user") ){
          if( p->user || p->del_user ){
            rc = DBSQL_ERROR;
          }else{
            box_geom_ctx_t *bg_ctx = dbsql_malloc(sizeof(box_geom_ctx_t));
            if( bg_ctx==0 ){
              rc = DBSQL_NOMEM;
            }else{
              bg_ctx->interp = interp;
              bg_ctx->script = Tcl_DuplicateObj(res);
              Tcl_IncrRefCount(bg_ctx->script);
              Tcl_ListObjReplace(interp, bg_ctx->script, 0, 1, 0, 0);
              p->user = (void*)bg_ctx;
              p->del_user = __test_del_user;
            }
          }
        }
        else if( 0==dbsql_stricmp(cmd, "user_is_zero") ){
          if( p->user || p->del_user ) rc = DBSQL_ERROR;
        }
      }
    }
  }
  return rc;
}

/*
# EVIDENCE-OF: R-00693-36727 The legacy geom callback is invoked with
# four arguments.

# EVIDENCE-OF: R-50437-53270 The first argument is a pointer to an
# dbsql_rtree_geometry_t structure which provides information about how
# the SQL function was invoked.

# EVIDENCE-OF: R-00090-24248 The third argument, coords[], is an array
# of n_coord coordinates that defines a bounding box to be tested.

# EVIDENCE-OF: R-28207-40885 The last argument is a pointer into which
# the callback result should be written.

*/
static int box_geom(
  dbsql_rtree_geometry_t *p,      /* R-50437-53270 */
  int n_coord,                     /* R-02424-24769 */
  dbsql_rtree_dbl_t *coords,      /* R-00090-24248 */
  int *res                       /* R-28207-40885 */
){
  int ii;

  if( p->n_param!=n_coord ){
    __invoke_tcl_geom_cb("box", p, n_coord, coords);
    return DBSQL_ERROR;
  }
  if( __invoke_tcl_geom_cb("box", p, n_coord, coords) ) return DBSQL_ERROR;

  for(ii=0; ii<n_coord; ii+=2){
    if( coords[ii]>p->params[ii+1] || coords[ii+1]<p->params[ii] ){
      /* R-28207-40885 */
      *res = 0;
      return DBSQL_OK;
    }
  }

  /* R-28207-40885 */
  *res = 1;

  return DBSQL_OK;
}

static int DBSQL_TCLAPI register_box_geom(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  extern const char *__err_name(int);
  DBSQL *db;
  box_geom_ctx_t *ctx;
  char ptrs[64];

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB SCRIPT");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  ctx = (box_geom_ctx_t*)ckalloc(sizeof(box_geom_ctx_t*));
  ctx->interp = interp;
  ctx->script = Tcl_DuplicateObj(objv[2]);
  Tcl_IncrRefCount(ctx->script);

  dbsql_rtree_geometry_callback(db, "box", box_geom, (void*)ctx);

  dbsql_snprintf(64, ptrs, "%p", (void*)ctx);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(ptrs, -1));
  return TCL_OK;
}

static int box_query(dbsql_rtree_query_info_t *info){
  const char *parent_withins[] = {"not", "partly", "fully", 0};
  box_query_ctx_t *ctx = (box_query_ctx_t*)info->context;
  Tcl_Interp *interp = ctx->interp;
  Tcl_Obj *eval;
  Tcl_Obj *arg;
  Tcl_Obj *tmp = 0;
  int rc;
  int ii;

  eval = Tcl_DuplicateObj(ctx->script);
  Tcl_IncrRefCount(eval);
  arg = Tcl_NewObj();
  Tcl_IncrRefCount(arg);

  /* params[] */
  tmp = Tcl_NewObj();
  Tcl_IncrRefCount(tmp);
  for(ii=0; ii<info->n_param; ii++){
    Tcl_Obj *p = Tcl_NewDoubleObj(info->params[ii]);
    Tcl_ListObjAppendElement(interp, tmp, p);
  }
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("aParam", -1));
  Tcl_ListObjAppendElement(interp, arg, tmp);
  Tcl_DecrRefCount(tmp);

  /* coords[] */
  tmp = Tcl_NewObj();
  Tcl_IncrRefCount(tmp);
  for(ii=0; ii<info->n_coord; ii++){
    Tcl_Obj *p = Tcl_NewDoubleObj(info->coords[ii]);
    Tcl_ListObjAppendElement(interp, tmp, p);
  }
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("aCoord", -1));
  Tcl_ListObjAppendElement(interp, arg, tmp);
  Tcl_DecrRefCount(tmp);

  /* an_queue[] */
  tmp = Tcl_NewObj();
  Tcl_IncrRefCount(tmp);
  for(ii=0; ii<=info->mx_level; ii++){
    Tcl_Obj *p = Tcl_NewIntObj((int)info->an_queue[ii]);
    Tcl_ListObjAppendElement(interp, tmp, p);
  }
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("anQueue", -1));
  Tcl_ListObjAppendElement(interp, arg, tmp);
  Tcl_DecrRefCount(tmp);
  
  /* i_level */
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("iLevel", -1));
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewIntObj(info->i_level));

  /* mx_level */
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("mxLevel", -1));
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewIntObj(info->mx_level));

  /* i_rowid */
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("iRowid", -1));
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewWideIntObj(info->i_rowid));

  /* r_parent_score */
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("rParentScore", -1));
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewDoubleObj(info->r_parent_score));

  /* parent_within */
  assert( info->parent_within==0 
       || info->parent_within==1 
       || info->parent_within==2 
  );
  Tcl_ListObjAppendElement(interp, arg, Tcl_NewStringObj("eParentWithin", -1));
  Tcl_ListObjAppendElement(interp, arg, 
      Tcl_NewStringObj(parent_withins[info->parent_within], -1)
  );

  Tcl_ListObjAppendElement(interp, eval, arg);
  rc = Tcl_EvalObjEx(interp, eval, 0) ? DBSQL_ERROR : DBSQL_OK;

  if( rc==DBSQL_OK ){
    double r_score = 0.0;
    TCL_SIZE n_obj = 0;
    int e_p = 0;
    Tcl_Obj **objs = 0;
    Tcl_Obj *res = Tcl_GetObjResult(interp);

    if( Tcl_ListObjGetElements(interp, res, &n_obj, &objs) 
     || n_obj!=2 
     || Tcl_GetDoubleFromObj(interp, objs[1], &r_score)
     || Tcl_GetIndexFromObj(interp, objs[0], parent_withins, "value", 0, &e_p)
    ){
      rc = DBSQL_ERROR;
    }else{
      info->r_score = r_score;
      info->parent_within = e_p;
    }
  }

  Tcl_DecrRefCount(arg);
  Tcl_DecrRefCount(eval);
  return rc;
}

static void box_query_destroy(void *p){
  box_query_ctx_t *ctx = (box_query_ctx_t*)p;
  Tcl_DecrRefCount(ctx->script);
  ckfree((char*)ctx);
}

static int DBSQL_TCLAPI register_box_query(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  extern const char *__err_name(int);
  DBSQL *db;
  box_query_ctx_t *ctx;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB SCRIPT");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  ctx = (box_query_ctx_t*)ckalloc(sizeof(box_query_ctx_t));
  ctx->interp = interp;
  ctx->script = Tcl_DuplicateObj(objv[2]);
  Tcl_IncrRefCount(ctx->script);

  dbsql_rtree_query_callback(
      db, "qbox", box_query, (void*)ctx, box_query_destroy
  );

  Tcl_ResetResult(interp);
  return TCL_OK;
}
#endif /* DBSQL_ENABLE_RTREE */


int Dbsqltestrtreedoc_Init(Tcl_Interp *interp){
#ifdef DBSQL_ENABLE_RTREE
  Tcl_CreateObjCommand(interp, "register_box_geom", register_box_geom, 0, 0);
  Tcl_CreateObjCommand(interp, "register_box_query", register_box_query, 0, 0);
#endif /* DBSQL_ENABLE_RTREE */
  return TCL_OK;
}
