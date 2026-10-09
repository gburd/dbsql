/*
** 2019 April 02
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
#if DBSQL_TEST          /* This file is used for testing only */

#include "dbsql.h"
#include "dbsql_int.h"
#include "tcl_dbsql.h"

#ifdef DBSQL_VDBE_COVERAGE

static u_int8_t __a_branch_array[200000];

static void test_vdbe_branch(
  void *ctx, 
  unsigned int i_src, 
  unsigned char i_branch, 
  unsigned char i_type
){
  if( i_src<sizeof(__a_branch_array) ){
    __a_branch_array[i_src] |= i_branch;
  }
}

static void __append_to_list(
  Tcl_Obj *list, 
  int i_line, 
  int i_path, 
  const char *never
){
  Tcl_Obj *p_new = Tcl_NewObj();
  Tcl_IncrRefCount(p_new);
  Tcl_ListObjAppendElement(0, p_new, Tcl_NewIntObj(i_line));
  Tcl_ListObjAppendElement(0, p_new, Tcl_NewIntObj(i_path));
  Tcl_ListObjAppendElement(0, p_new, Tcl_NewStringObj(never, -1));
  Tcl_ListObjAppendElement(0, list, p_new);
  Tcl_DecrRefCount(p_new);
}


static int DBSQL_TCLAPI test_vdbe_coverage(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *subs[] = { "start", "report", "stop", 0 };
  int i_sub = -1;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "sub-command");
    return TCL_ERROR;
  }

  if( Tcl_GetIndexFromObj(interp, objv[1], subs, "sub-command", 0, &i_sub) ){
    return TCL_ERROR;
  }

  Tcl_ResetResult(interp);
  assert( i_sub==0 || i_sub==1 || i_sub==2 );
  switch( i_sub ){
    case 0:       /* start */
      memset(__a_branch_array, 0, sizeof(__a_branch_array));
      dbsql_test_control(DBSQL_TESTCTRL_VDBE_COVERAGE, test_vdbe_branch, 0);
      break;
    case 1: {     /* report */
      int i;
      Tcl_Obj *res = Tcl_NewObj();
      Tcl_IncrRefCount(res);
      for(i=0; i<sizeof(__a_branch_array); i++){
        u_int8_t b = __a_branch_array[i];
        int flag = ((b >> 4)==4);
        if( b ){
          if( (b & 0x01)==0 ){
            __append_to_list(res, i, 0, flag ? "less than" : "falls through");
          }
          if( (b & 0x02)==0 ){
            __append_to_list(res, i, 1, flag ? "equal" : "taken");
          }
          if( (b & 0x04)==0 ){
            __append_to_list(res, i, 2, flag ? "greater-than" : "NULL");
          }
        }
      }
      Tcl_SetObjResult(interp, res);
      Tcl_DecrRefCount(res);
      break;
    };
      
    default:      /* stop */         
      dbsql_test_control(DBSQL_TESTCTRL_VDBE_COVERAGE, 0, 0);
      break;
  }

  return TCL_OK;
}

#endif  /* DBSQL_VDBE_COVERAGE */

int Dbsqltestvdbecov_Init(Tcl_Interp *interp){
#ifdef DBSQL_VDBE_COVERAGE
  Tcl_CreateObjCommand(interp, "vdbe_coverage", test_vdbe_coverage, 0, 0);
#endif
  return TCL_OK;
}

#endif
