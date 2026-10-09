/*
** 2009 November 10
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
** This file implements a read-only VIRTUAL TABLE that contains the
** content of a C-language array of integer values.  See the corresponding
** header file for full details.
**
** This virtual table is used for internal testing of dbsql only.  It is
** not recommended for use in production.  For a similar virtual table that
** is production-ready, see the "carray" virtual table over in ext/misc.
*/
#include "test_intarray.h"
#include <string.h>
#include <assert.h>


/*
** Definition of the dbsql_intarray object.
**
** The internal representation of an intarray object is subject
** to change, is not externally visible, and should be used by
** the implementation of intarray only.  This object is opaque
** to users.
*/
struct dbsql_intarray {
  int n;                    /* Number of elements in the array */
  int64_t *a;         /* Contents of the array */
  void (*__x_free)(void*);     /* Function used to free a[] */
};

/* Objects used internally by the virtual table implementation */
typedef struct intarray_vtab intarray_vtab;
typedef struct intarray_cursor intarray_cursor;

/* An intarray table object */
struct intarray_vtab {
  dbsql_vtab_t base;            /* Base class */
  dbsql_intarray *content;   /* Content of the integer array */
};

/* An intarray cursor object */
struct intarray_cursor {
  dbsql_vtab_cursor_t base;    /* Base class */
  int i;                       /* Current cursor position */
};

/*
** None of this works unless we have virtual tables.
*/
#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Free an dbsql_intarray object.
*/
static void __intarray_free(void *x){
  dbsql_intarray *p = (dbsql_intarray*)x;
  if( p->__x_free ){
    p->__x_free(p->a);
  }
  dbsql_free(p);
}

/*
** table_t destructor for the intarray module.
*/
static int __intarray_destroy(dbsql_vtab_t *p){
  intarray_vtab *vtab = (intarray_vtab*)p;
  dbsql_free(vtab);
  return 0;
}

/*
** table_t constructor for the intarray module.
*/
static int __intarray_create(
  DBSQL *db,              /* Database where module is created */
  void *aux,               /* clientdata for the module */
  int argc,                 /* Number of arguments */
  const char *const*argv,   /* Value for all arguments */
  dbsql_vtab_t **pp_vtab,    /* Write the new virtual table object here */
  char **err_p              /* Put error message text here */
){
  int rc = DBSQL_NOMEM;
  intarray_vtab *vtab = dbsql_malloc64(sizeof(intarray_vtab));

  if( vtab ){
    memset(vtab, 0, sizeof(intarray_vtab));
    vtab->content = (dbsql_intarray*)aux;
    rc = dbsql_declare_vtab(db, "CREATE TABLE x(value INTEGER PRIMARY KEY)");
  }
  *pp_vtab = (dbsql_vtab_t *)vtab;
  return rc;
}

/*
** Open a new cursor on the intarray table.
*/
static int __intarray_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  int rc = DBSQL_NOMEM;
  intarray_cursor *cur;
  cur = dbsql_malloc64(sizeof(intarray_cursor));
  if( cur ){
    memset(cur, 0, sizeof(intarray_cursor));
    *cursor = (dbsql_vtab_cursor_t *)cur;
    rc = DBSQL_OK;
  }
  return rc;
}

/*
** Close a intarray table cursor.
*/
static int __intarray_close(dbsql_vtab_cursor_t *cur){
  intarray_cursor *p_cur = (intarray_cursor *)cur;
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Retrieve a column of data.
*/
static int __intarray_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  intarray_cursor *p_cur = (intarray_cursor*)cur;
  intarray_vtab *vtab = (intarray_vtab*)cur->vtab;
  if( p_cur->i>=0 && p_cur->i<vtab->content->n ){
    dbsql_result_int64(ctx, vtab->content->a[p_cur->i]);
  }
  return DBSQL_OK;
}

/*
** Retrieve the current rowid.
*/
static int __intarray_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  intarray_cursor *p_cur = (intarray_cursor *)cur;
  *rowid = p_cur->i;
  return DBSQL_OK;
}

static int __intarray_eof(dbsql_vtab_cursor_t *cur){
  intarray_cursor *p_cur = (intarray_cursor *)cur;
  intarray_vtab *vtab = (intarray_vtab *)cur->vtab;
  return p_cur->i>=vtab->content->n;
}

/*
** Advance the cursor to the next row.
*/
static int __intarray_next(dbsql_vtab_cursor_t *cur){
  intarray_cursor *p_cur = (intarray_cursor *)cur;
  p_cur->i++;
  return DBSQL_OK;
}

/*
** Reset a intarray table cursor.
*/
static int __intarray_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  intarray_cursor *cur = (intarray_cursor *)vtab_cursor;
  cur->i = 0;
  return DBSQL_OK;
}

/*
** Analyse the WHERE condition.
*/
static int __intarray_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  return DBSQL_OK;
}

/*
** A virtual table module that merely echos method calls into TCL
** variables.
*/
static dbsql_module_t __intarray_module = {
  0,                           /* i_version */
  __intarray_create,              /* create - create a new virtual table */
  __intarray_create,              /* connect - connect to an existing vtab */
  __intarray_best_index,           /* best_index - find the best query index */
  __intarray_destroy,             /* disconnect - disconnect a vtab */
  __intarray_destroy,             /* destroy - destroy a vtab */
  __intarray_open,                /* x_open - open a cursor */
  __intarray_close,               /* x_close - close a cursor */
  __intarray_filter,              /* filter - configure scan constraints */
  __intarray_next,                /* next - advance a cursor */
  __intarray_eof,                 /* eof */
  __intarray_column,              /* column - read data */
  __intarray_rowid,               /* rowid - read data */
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

#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) */

/*
** Invoke this routine to create a specific instance of an intarray object.
** The new intarray object is returned by the 3rd parameter.
**
** Each intarray object corresponds to a virtual table in the TEMP table
** with a name of name.
**
** Destroy the intarray object by dropping the virtual table.  If not done
** explicitly by the application, the virtual table will be dropped implicitly
** by the system when the database connection is closed.
*/
DBSQL_API int dbsql_intarray_create(
  DBSQL *db,
  const char *name,
  dbsql_intarray **pp_return
){
  int rc = DBSQL_OK;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_intarray *p;

  *pp_return = p = dbsql_malloc64( sizeof(*p) );
  if( p==0 ){
    return DBSQL_NOMEM;
  }
  memset(p, 0, sizeof(*p));
  rc = dbsql_create_module_v2(db, name, &__intarray_module, p,
                                (void(*)(void*))__intarray_free);
  if( rc==DBSQL_OK ){
    char *sql;
    sql = dbsql_mprintf("CREATE VIRTUAL TABLE temp.%Q USING %Q",
                           name, name);
    rc = dbsql_exec(db, sql, 0, 0, 0);
    dbsql_free(sql);
  }
#endif
  return rc;
}

/*
** Bind a new array array of integers to a specific intarray object.
**
** The array of integers bound must be unchanged for the duration of
** any query against the corresponding virtual table.  If the integer
** array does change or is deallocated undefined behavior will result.
*/
DBSQL_API int dbsql_intarray_bind(
  dbsql_intarray *int_array,   /* The intarray object to bind to */
  int n_elements,                 /* Number of elements in the intarray */
  int64_t *elements,      /* Content of the intarray */
  void (*__x_free)(void*)           /* How to dispose of the intarray when done */
){
  if( int_array->__x_free ){
    int_array->__x_free(int_array->a);
  }
  int_array->n = n_elements;
  int_array->a = elements;
  int_array->__x_free = __x_free;
  return DBSQL_OK;
}


/*****************************************************************************
** Everything below is interface for testing this module.
*/
#ifdef DBSQL_TEST
#include "tcl_dbsql.h"

/*
** Routines to encode and decode pointers
*/
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);
extern void *__test_text_to_ptr(const char*);
extern int __test_make_pointer_str(Tcl_Interp*, char *ptr, void*);
extern const char *__err_name(int);

/*
**    dbsql_intarray_create  DB  NAME
**
** Invoke the dbsql_intarray_create interface.  A string that becomes
** the first parameter to dbsql_intarray_bind.
*/
static int DBSQL_TCLAPI test_intarray_create(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  DBSQL *db;
  const char *name;
  dbsql_intarray *array;
  int rc = DBSQL_OK;
  char ptr[100];

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  name = Tcl_GetString(objv[2]);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_intarray_create(db, name, &array);
#endif
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), (char*)0);
    return TCL_ERROR;
  }
  __test_make_pointer_str(interp, ptr, array);
  Tcl_AppendResult(interp, ptr, (char*)0);
  return TCL_OK;
}

/*
**    dbsql_intarray_bind  INTARRAY  ?VALUE ...?
**
** Invoke the dbsql_intarray_bind interface on the given array of integers.
*/
static int DBSQL_TCLAPI test_intarray_bind(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  dbsql_intarray *array;
  int rc = DBSQL_OK;
  int i, n;
  int64_t *a;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "INTARRAY");
    return TCL_ERROR;
  }
  array = (dbsql_intarray*)__test_text_to_ptr(Tcl_GetString(objv[1]));
  n = objc - 2;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  a = dbsql_malloc64( sizeof(a[0])*n );
  if( a==0 ){
    Tcl_AppendResult(interp, "DBSQL_NOMEM", (char*)0);
    return TCL_ERROR;
  }
  for(i=0; i<n; i++){
    Tcl_WideInt x = 0;
    Tcl_GetWideIntFromObj(0, objv[i+2], &x);
    a[i] = x;
  }
  rc = dbsql_intarray_bind(array, n, a, dbsql_free);
  if( rc!=DBSQL_OK ){
    Tcl_AppendResult(interp, __err_name(rc), (char*)0);
    return TCL_ERROR;
  }
#endif
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltestintarray_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "dbsql_intarray_create", test_intarray_create, 0 },
     { "dbsql_intarray_bind", test_intarray_bind, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
  return TCL_OK;
}

#endif /* DBSQL_TEST */
