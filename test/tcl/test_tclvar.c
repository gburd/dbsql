/*
** 2006 June 13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table interfaces.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library.
**
** The emphasis of this file is a virtual table that provides
** access to TCL variables.
**
** The TCLVAR eponymous virtual table has a schema like this:
**
**    CREATE TABLE tclvar(
**       name TEXT,       -- base name of the variable:  "x" in "$x(y)"
**       arrayname TEXT,  -- array index name: "y" in "$x(y)"
**       value TEXT,      -- the value of the variable 
**       fullname TEXT,   -- the full name of the variable
**       PRIMARY KEY(fullname)
**    ) WITHOUT ROWID;
**
** DELETE, INSERT, and UPDATE operations use the "fullname" field to
** determine the variable to be modified.  Changing "value" to NULL
** deletes the variable.
**
** For SELECT operations, the "name" and "arrayname" fields will always
** match the "fullname" field.  For DELETE, INSERT, and UPDATE, the
** "name" and "arrayname" fields are ignored and the variable is modified
** according to "fullname" and "value" only.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Characters that make up the idx_str created by best_index for filter.
*/
#define TCLVAR_NAME_EQ      'e'
#define TCLVAR_NAME_MATCH   'm'
#define TCLVAR_VALUE_GLOB   'g'
#define TCLVAR_VALUE_REGEXP 'r'
#define TCLVAR_VALUE_LIKE   'l'

typedef struct tclvar_vtab tclvar_vtab;
typedef struct tclvar_cursor tclvar_cursor;

/* 
** A tclvar virtual-table object 
*/
struct tclvar_vtab {
  dbsql_vtab_t base;
  Tcl_Interp *interp;
};

/* A tclvar cursor object */
struct tclvar_cursor {
  dbsql_vtab_cursor_t base;

  Tcl_Obj *list1;     /* Result of [info vars ?pattern?] */
  Tcl_Obj *list2;     /* Result of [array names [lindex $list1 $i1]] */
  TCL_SIZE i1;              /* Current item in list1 */
  TCL_SIZE i2;              /* Current item (if any) in list2 */
};

/* Methods for the tclvar module */
static int __tclvar_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  tclvar_vtab *vtab;
  static const char schema[] = 
     "CREATE TABLE x("
     "  name TEXT,"                       /* Base name */
     "  arrayname TEXT,"                  /* Array index */
     "  value TEXT,"                      /* Value */
     "  fullname TEXT PRIMARY KEY"        /* base(index) name */
     ") WITHOUT ROWID";
  vtab = __malloc_zero( sizeof(*vtab) );
  if( vtab==0 ) return DBSQL_NOMEM;
  *pp_vtab = &vtab->base;
  vtab->interp = (Tcl_Interp *)aux;
  dbsql_declare_vtab(db, schema);
  return DBSQL_OK;
}
/* Note that for this virtual table, the create and connect
** methods are identical. */

static int __tclvar_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}
/* The disconnect and destroy methods are also the same */

/*
** Open a new tclvar cursor.
*/
static int __tclvar_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  tclvar_cursor *cur;
  cur = __malloc_zero(sizeof(tclvar_cursor));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Close a tclvar cursor.
*/
static int __tclvar_close(dbsql_vtab_cursor_t *cur){
  tclvar_cursor *p_cur = (tclvar_cursor *)cur;
  if( p_cur->list1 ){
    Tcl_DecrRefCount(p_cur->list1);
  }
  if( p_cur->list2 ){
    Tcl_DecrRefCount(p_cur->list2);
  }
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Returns 1 if data is ready, or 0 if not.
*/
static int next2(Tcl_Interp *interp, tclvar_cursor *cur, Tcl_Obj *obj){
  Tcl_Obj *p;

  if( obj ){
    if( !cur->list2 ){
      p = Tcl_NewStringObj("array names", -1);
      Tcl_IncrRefCount(p);
      Tcl_ListObjAppendElement(0, p, obj);
      Tcl_EvalObjEx(interp, p, TCL_EVAL_GLOBAL);
      Tcl_DecrRefCount(p);
      cur->list2 = Tcl_GetObjResult(interp);
      Tcl_IncrRefCount(cur->list2);
      assert( cur->i2==0 );
    }else{
      TCL_SIZE n = 0;
      cur->i2++;
      Tcl_ListObjLength(0, cur->list2, &n);
      if( cur->i2>=n ){
        Tcl_DecrRefCount(cur->list2);
        cur->list2 = 0;
        cur->i2 = 0;
        return 0;
      }
    }
  }

  return 1;
}

static int __tclvar_next(dbsql_vtab_cursor_t *cur){
  Tcl_Obj *obj;
  TCL_SIZE n = 0;
  int ok = 0;

  tclvar_cursor *p_cur = (tclvar_cursor *)cur;
  Tcl_Interp *interp = ((tclvar_vtab *)(cur->vtab))->interp;

  Tcl_ListObjLength(0, p_cur->list1, &n);
  while( !ok && p_cur->i1<n ){
    Tcl_ListObjIndex(0, p_cur->list1, p_cur->i1, &obj);
    ok = next2(interp, p_cur, obj);
    if( !ok ){
      p_cur->i1++;
    }
  }

  return 0;
}

static int __tclvar_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  tclvar_cursor *cur = (tclvar_cursor *)vtab_cursor;
  Tcl_Interp *interp = ((tclvar_vtab *)(vtab_cursor->vtab))->interp;
  Tcl_Obj *p = Tcl_NewStringObj("tclvar_filter_cmd", -1);

  const char *eq = "";
  const char *match = "";
  const char *glob = "";
  const char *regexp = "";
  const char *like = "";
  int i;

  for(i=0; idx_str[i]; i++){
    switch( idx_str[i] ){
      case TCLVAR_NAME_EQ:
        eq = (const char*)dbsql_value_text(argv[i]);
        break;
      case TCLVAR_NAME_MATCH:
        match = (const char*)dbsql_value_text(argv[i]);
        break;
      case TCLVAR_VALUE_GLOB:
        glob = (const char*)dbsql_value_text(argv[i]);
        break;
      case TCLVAR_VALUE_REGEXP:
        regexp = (const char*)dbsql_value_text(argv[i]);
        break;
      case TCLVAR_VALUE_LIKE:
        like = (const char*)dbsql_value_text(argv[i]);
        break;
      default:
        assert( 0 );
    }
  }

  Tcl_IncrRefCount(p);
  Tcl_ListObjAppendElement(0, p, Tcl_NewStringObj(eq, -1));
  Tcl_ListObjAppendElement(0, p, Tcl_NewStringObj(match, -1));
  Tcl_ListObjAppendElement(0, p, Tcl_NewStringObj(glob, -1));
  Tcl_ListObjAppendElement(0, p, Tcl_NewStringObj(regexp, -1));
  Tcl_ListObjAppendElement(0, p, Tcl_NewStringObj(like, -1));

  Tcl_EvalObjEx(interp, p, TCL_EVAL_GLOBAL);
  if( cur->list1 ){
    Tcl_DecrRefCount(cur->list1);
  }
  if( cur->list2 ){
    Tcl_DecrRefCount(cur->list2);
    cur->list2 = 0;
  }
  cur->i1 = 0;
  cur->i2 = 0;
  cur->list1 = Tcl_GetObjResult(interp);
  Tcl_IncrRefCount(cur->list1);

  Tcl_DecrRefCount(p);
  return __tclvar_next(vtab_cursor);
}

static int __tclvar_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  Tcl_Obj *p1;
  Tcl_Obj *p2;
  const char *z1; 
  const char *z2 = "";
  tclvar_cursor *p_cur = (tclvar_cursor*)cur;
  Tcl_Interp *interp = ((tclvar_vtab *)cur->vtab)->interp;

  Tcl_ListObjIndex(interp, p_cur->list1, p_cur->i1, &p1);
  Tcl_ListObjIndex(interp, p_cur->list2, p_cur->i2, &p2);
  z1 = Tcl_GetString(p1);
  if( p2 ){
    z2 = Tcl_GetString(p2);
  }
  switch (i) {
    case 0: {
      dbsql_result_text(ctx, z1, -1, DBSQL_TRANSIENT);
      break;
    }
    case 1: {
      dbsql_result_text(ctx, z2, -1, DBSQL_TRANSIENT);
      break;
    }
    case 2: {
      Tcl_Obj *val = Tcl_GetVar2Ex(interp, z1, *z2?z2:0, TCL_GLOBAL_ONLY);
      dbsql_result_text(ctx, Tcl_GetString(val), -1, DBSQL_TRANSIENT);
      break;
    }
    case 3: {
      char *z3;
      if( p2 ){
        z3 = dbsql_mprintf("%s(%s)", z1, z2);
        dbsql_result_text(ctx, z3, -1, dbsql_free);
      }else{
        dbsql_result_text(ctx, z1, -1, DBSQL_TRANSIENT);
      }
      break;
    }
  }
  return DBSQL_OK;
}

static int __tclvar_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  *rowid = 0;
  return DBSQL_OK;
}

static int __tclvar_eof(dbsql_vtab_cursor_t *cur){
  tclvar_cursor *p_cur = (tclvar_cursor*)cur;
  return (p_cur->list2?0:1);
}

/*
** If nul-terminated string str does not already contain the character 
** passed as the second argument, append it and return 0. Or, if there is
** already an instance of x in str, do nothing return 1;
**
** There is guaranteed to be enough room in the buffer pointed to by str
** for the new character and nul-terminator.
*/
static int __tclvar_add_to_idxstr(char *str, char x){
  int i;
  for(i=0; str[i]; i++){
    if( str[i]==x ) return 1;
  }
  str[i] = x;
  str[i+1] = '\0';
  return 0;
}

/*
** Return true if variable $::tclvar_set_omit exists and is set to true.
** False otherwise.
*/
static int __tclvar_set_omit(Tcl_Interp *interp){
  int rc;
  int res = 0;
  Tcl_Obj *p_res;
  rc = Tcl_Eval(interp,
    "expr {[info exists ::tclvar_set_omit] && $::tclvar_set_omit}"
  );
  if( rc==TCL_OK ){
    p_res = Tcl_GetObjResult(interp);
    rc = Tcl_GetBooleanFromObj(0, p_res, &res);
  }
  return (rc==TCL_OK && res);
}

/*
** The best_index() method. This virtual table supports the following
** operators:
**
**     name = ?                    (omit flag clear)
**     name MATCH ?                (omit flag set)
**     value GLOB ?                (omit flag set iff $::tclvar_set_omit)
**     value REGEXP ?              (omit flag set iff $::tclvar_set_omit)
**     value LIKE ?                (omit flag set iff $::tclvar_set_omit)
**
** For each constraint present, the corresponding TCLVAR_XXX character is
** appended to the idx_str value. 
*/
static int __tclvar_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  tclvar_vtab *p_tab = (tclvar_vtab*)tab;
  int ii;
  char *str = dbsql_malloc(32);
  int i_str = 0;

  if( str==0 ) return DBSQL_NOMEM;
  str[0] = '\0';

  for(ii=0; ii<idx_info->n_constraint; ii++){
    struct dbsql_index_constraint_t const *cons = &idx_info->constraints[ii];
    struct dbsql_index_constraint_usage_t *usage;
    
    usage = &idx_info->constraint_usages[ii];
    if( cons->usable ){
      /* name = ? */
      if( cons->op==DBSQL_INDEX_CONSTRAINT_EQ && cons->i_column==0 ){
        if( 0==__tclvar_add_to_idxstr(str, TCLVAR_NAME_EQ) ){
          usage->argv_index = ++i_str;
          usage->omit = 0;
        }
      }

      /* name MATCH ? */
      if( cons->op==DBSQL_INDEX_CONSTRAINT_MATCH && cons->i_column==0 ){
        if( 0==__tclvar_add_to_idxstr(str, TCLVAR_NAME_MATCH) ){
          usage->argv_index = ++i_str;
          usage->omit = 1;
        }
      }

      /* value GLOB ? */
      if( cons->op==DBSQL_INDEX_CONSTRAINT_GLOB && cons->i_column==2 ){
        if( 0==__tclvar_add_to_idxstr(str, TCLVAR_VALUE_GLOB) ){
          usage->argv_index = ++i_str;
          usage->omit = __tclvar_set_omit(p_tab->interp);
        }
      }

      /* value REGEXP ? */
      if( cons->op==DBSQL_INDEX_CONSTRAINT_REGEXP && cons->i_column==2 ){
        if( 0==__tclvar_add_to_idxstr(str, TCLVAR_VALUE_REGEXP) ){
          usage->argv_index = ++i_str;
          usage->omit = __tclvar_set_omit(p_tab->interp);
        }
      }

      /* value LIKE ? */
      if( cons->op==DBSQL_INDEX_CONSTRAINT_LIKE && cons->i_column==2 ){
        if( 0==__tclvar_add_to_idxstr(str, TCLVAR_VALUE_LIKE) ){
          usage->argv_index = ++i_str;
          usage->omit = __tclvar_set_omit(p_tab->interp);
        }
      }
    }
  }
  idx_info->idx_str = str;
  idx_info->need_to_free_idx_str = 1;

  return DBSQL_OK;
}

/*
** Invoked for any UPDATE, INSERT, or DELETE against a tclvar table
*/
static int __tclvar_update(
  dbsql_vtab_t *tab,
  int argc,
  dbsql_value_t **argv,
  int64_t *rowid
){
  tclvar_vtab *p_tab = (tclvar_vtab*)tab;
  if( argc==1 ){
    /* A DELETE operation.  The variable to be deleted is stored in argv[0] */
    const char *var = (const char*)dbsql_value_text(argv[0]);
    Tcl_UnsetVar(p_tab->interp, var, TCL_GLOBAL_ONLY);
    return DBSQL_OK;
  }
  if( dbsql_value_type(argv[0])==DBSQL_NULL ){
    /* An INSERT operation */
    const char *value = (const char*)dbsql_value_text(argv[4]);
    const char *name;
    if( dbsql_value_type(argv[5])!=DBSQL_TEXT ){
      tab->errmsg = dbsql_mprintf("the 'fullname' column must be TEXT");
      return DBSQL_ERROR;
    }
    name = (const char*)dbsql_value_text(argv[5]);
    if( value ){
      Tcl_SetVar(p_tab->interp, name, value, TCL_GLOBAL_ONLY);
    }else{
      Tcl_UnsetVar(p_tab->interp, name, TCL_GLOBAL_ONLY);
    }
    return DBSQL_OK;
  }
  if( dbsql_value_type(argv[0])==DBSQL_TEXT
   && dbsql_value_type(argv[1])==DBSQL_TEXT
  ){
    /* An UPDATE operation */
    const char *old_name = (const char*)dbsql_value_text(argv[0]);
    const char *new_name = (const char*)dbsql_value_text(argv[1]);
    const char *value = (const char*)dbsql_value_text(argv[4]);

    if( strcmp(old_name, new_name)!=0 || value==0 ){
      Tcl_UnsetVar(p_tab->interp, old_name, TCL_GLOBAL_ONLY);
    }
    if( value!=0 ){
      Tcl_SetVar(p_tab->interp, new_name, value, TCL_GLOBAL_ONLY);
    }
    return DBSQL_OK;
  }
  tab->errmsg = dbsql_mprintf("prohibited TCL variable change");
  return DBSQL_ERROR;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static dbsql_module_t __tclvar_module = {
  0,                         /* i_version */
  __tclvar_connect,
  __tclvar_connect,
  __tclvar_best_index,
  __tclvar_disconnect, 
  __tclvar_disconnect,
  __tclvar_open,                  /* x_open - open a cursor */
  __tclvar_close,                 /* x_close - close a cursor */
  __tclvar_filter,                /* filter - configure scan constraints */
  __tclvar_next,                  /* next - advance a cursor */
  __tclvar_eof,                   /* eof - check for end of scan */
  __tclvar_column,                /* column - read data */
  __tclvar_rowid,                 /* rowid - read data */
  __tclvar_update,                /* update */
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

/*
** Decode a pointer to an DBSQL object.
*/
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);

/*
** Register the echo virtual table module.
*/
static int DBSQL_TCLAPI register_tclvar_module(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  int rc = TCL_OK;
  DBSQL *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_create_module(db, "tclvar", &__tclvar_module, (void*)interp);
  rc = Tcl_Eval(interp, 
      "proc like {pattern str} {\n"
      "  set p [string map {% * _ ?} $pattern]\n"
      "  string match $p $str\n"
      "}\n"
      "proc tclvar_filter_cmd {eq match glob regexp like} {\n"
      "  set res {}\n"
      "  set pattern $eq\n"
      "  if {$pattern=={}} { set pattern $match }\n"
      "  if {$pattern=={}} { set pattern * }\n"
      "  foreach v [uplevel #0 info vars $pattern] {\n"
      "    if {($glob=={} || [string match $glob [uplevel #0 set $v]])\n"
      "     && ($like=={} || [like $like [uplevel #0 set $v]])\n"
      "     && ($regexp=={} || [regexp $regexp [uplevel #0 set $v]])\n"
      "    } {\n"
      "      lappend res $v\n"
      "    }\n"
      "  }\n"
      "  set res\n"
      "}\n"
  );
#endif
  return rc;
}

#endif


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltesttclvar_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "register_tclvar_module",   register_tclvar_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
