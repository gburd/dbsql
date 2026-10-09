/*
** 2007 May 05
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
#include "btree_int.h"
#include "tcl_dbsql.h"

/*
** Usage: dbsql_shared_cache_report
**
** Return a list of file that are shared and the number of
** references to each file.
*/
int DBSQL_TCLAPI __sm_shared_cache_report(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifndef DBSQL_OMIT_SHARED_CACHE
  extern sm_shared_t *__shared_cache_list;
  sm_shared_t *bt;
  Tcl_Obj *ret = Tcl_NewObj();
  for(bt=GLOBAL(sm_shared_t*,__shared_cache_list); bt; bt=bt->next){
    const char *file = __sm_pager_filename(bt->pager, 1);
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewStringObj(file, -1));
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewIntObj(bt->n_ref));
  }
  Tcl_SetObjResult(interp, ret);
#endif
  return TCL_OK;
}

/*
** Print debugging information about all cursors to standard output.
*/
void __sm_cursor_list(sm_t *p){
#ifdef DBSQL_DEBUG
  sm_cursor_t *cur;
  sm_shared_t *bt = p->bt;
  for(cur=bt->cursor; cur; cur=cur->next){
    __mem_page *page = cur->pages[cur->i_page];
    char *mode = (cur->cur_flags & __btcf_write_flag) ? "rw" : "ro";
    __debug_printf("CURSOR %p rooted at %4d(%s) currently at %d.%d%s\n",
       cur, cur->pgno_root, mode,
       page ? page->pgno : 0, cur->idx[cur->i_page],
       (cur->state==CURSOR_VALID) ? "" : " eof"
    );
  }
#endif
}
