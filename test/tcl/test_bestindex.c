/*
** 2016-03-01
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table best_index method and the query
** planner.
*/


/*
** INSTRUCTIONS
**
** This module exports a single tcl command - [register_tcl_module]. When
** invoked, it registers a special virtual table module with a database
** connection.
**
** The virtual table is currently read-only. And always returns zero rows.
** It is created with a single argument - the name of a Tcl command - as
** follows:
**
**   CREATE VIRTUAL TABLE x1 USING tcl(tcl_command);
**
** The command [tcl_command] is invoked when the table is first created (or
** connected), when the best_index() method is invoked and when the filter()
** method is called. When it is created (or connected), it is invoked as
** follows:
**
**   tcl_command connect
**
** In this case the return value of the script is passed to the
** dbsql_declare_vtab() function to create the virtual table schema.
**
** When the best_index() method is called by dbsql, the Tcl command is
** invoked as:
**
**   tcl_command best_index CONSTRAINTS ORDERBY MASK
**
** where CONSTRAINTS is a tcl representation of the constraints[] array,
** ORDERBY is a representation of the contents of the order_bies[] array and
** MASK is a copy of dbsql_index_info_t.col_used. For example if the virtual
** table is declared as:
**
**   CREATE TABLE x1(a, b, c)
**
** and the query is:
**
**   SELECT * FROM x1 WHERE a=? AND c<? ORDER BY b, c;
**
** then the Tcl command is:
**
**   tcl_command best_index                                  \
**     {{op eq column 0 usable 1} {op lt column 2 usable 1}} \
**     {{column 1 desc 0} {column 2 desc 0}}                 \
**     7
**
** The return value of the script is a list of key-value pairs used to
** populate the output fields of the dbsql_index_info_t structure. Possible
** keys and the usage of the accompanying values are:
** 
**   "orderby"          (value of order_by_consumed flag)
**   "cost"             (value of estimated_cost field)
**   "rows"             (value of estimated_rows field)
**   "use"              (index of used constraint in constraints[])
**   "omit"             (like "use", but also sets omit flag)
**   "idxnum"           (value of idx_num field)
**   "idxstr"           (value of idx_str field)
**
** Refer to code below for further details.
**
** When dbsql calls the filter() method, this module invokes the following
** Tcl script:
**
**   tcl_command filter IDXNUM IDXSTR ARGLIST
**
** IDXNUM and IDXSTR are the values of the idx_num and idx_str parameters
** passed to filter. ARGLIST is a Tcl list containing each of the arguments
** passed to filter in text form.
**
** As with best_index(), the return value of the script is interpreted as a
** list of key-value pairs. There is currently only one key defined - "sql".
** The value must be the full text of an SQL statement that returns the data
** for the current scan. The leftmost column returned by the SELECT is assumed
** to contain the rowid. Other columns must follow, in order from left to
** right.
*/


#include "dbsql_int.h"
#include "tcl_dbsql.h"

#ifndef DBSQL_OMIT_VIRTUALTABLE


typedef struct tcl_vtab tcl_vtab;
typedef struct tcl_cursor tcl_cursor;
typedef struct test_find_function_t test_find_function_t;
typedef struct test_vtab_context_t test_vtab_context_t;

/* 
** A fs virtual-table object 
*/
struct tcl_vtab {
  dbsql_vtab_t base;
  Tcl_Interp *interp;
  Tcl_Obj *cmd;
  test_find_function_t *find_function_list;
  DBSQL *db;
};

/* A tcl cursor object */
struct tcl_cursor {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *stmt;            /* Read data from here */
};

struct test_find_function_t {
  tcl_vtab *tab;
  const char *name;
  test_find_function_t *next;
};

struct test_vtab_context_t {
  Tcl_Interp *interp;
  Tcl_Obj *p_default;
};

/*
** Dequote string z in place.
*/
static void __tcl_dequote(char *z){
  char q = z[0];

  /* Set stack variable q to the close-quote character */
  if( q=='[' || q=='\'' || q=='"' || q=='`' ){
    int i_in = 1;
    int i_out = 0;
    if( q=='[' ) q = ']';  

    while( ALWAYS(z[i_in]) ){
      if( z[i_in]==q ){
        if( z[i_in+1]!=q ){
          /* Character i_in was the close quote. */
          i_in++;
          break;
        }else{
          /* Character i_in and i_in+1 form an escaped quote character. Skip
          ** the input cursor past both and copy a single quote character 
          ** to the output buffer. */
          i_in += 2;
          z[i_out++] = q;
        }
      }else{
        z[i_out++] = z[i_in++];
      }
    }

    z[i_out] = '\0';
  }
}

/*
** This function is the implementation of both the connect and create
** methods of the fs virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int __tcl_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  test_vtab_context_t *ctx = (test_vtab_context_t*)aux;
  Tcl_Interp *interp = ctx->interp;
  tcl_vtab *tab = 0;
  char *cmd = 0;
  Tcl_Obj *script = 0;
  int rc = DBSQL_OK;

  if( argc!=4 && (argc!=3 || ctx->p_default==0) ){
    *err_p = dbsql_mprintf("wrong number of arguments");
    return DBSQL_ERROR;
  }

  if( argc==4 ){
    cmd = dbsql_malloc64(strlen(argv[3])+1);
  }
  tab = (tcl_vtab*)dbsql_malloc64(sizeof(tcl_vtab));
  if( (cmd || argc==3) && tab ){
    memset(tab, 0, sizeof(tcl_vtab));

    if( cmd ){
      memcpy(cmd, argv[3], strlen(argv[3])+1);
      __tcl_dequote(cmd);
      tab->cmd = Tcl_NewStringObj(cmd, -1);
    }else{
      tab->cmd = Tcl_DuplicateObj(ctx->p_default);
    }

    tab->interp = interp;
    tab->db = db;
    Tcl_IncrRefCount(tab->cmd);

    script = Tcl_DuplicateObj(tab->cmd);
    Tcl_IncrRefCount(script);
    Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj("xConnect", -1));

    rc = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
    if( rc!=TCL_OK ){
      *err_p = dbsql_mprintf("%s", Tcl_GetStringResult(interp));
      if( dbsql_stricmp(*err_p, "database schema has changed")==0 ){
        rc = DBSQL_SCHEMA;
      }else{
        rc = DBSQL_ERROR;
      }
    }else{
      rc = dbsql_declare_vtab(db, Tcl_GetStringResult(interp));
      if( rc!=DBSQL_OK ){
        *err_p = dbsql_mprintf("declare_vtab: %s", dbsql_errmsg(db));
      }
    }

    if( rc!=DBSQL_OK ){
      dbsql_free(tab);
      tab = 0;
    }
  }else{
    rc = DBSQL_NOMEM;
  }

  dbsql_free(cmd);
  *vtab = tab ? &tab->base : 0;
  return rc;
}

/* The disconnect and destroy methods are also the same */
static int __tcl_disconnect(dbsql_vtab_t *vtab){
  tcl_vtab *tab = (tcl_vtab*)vtab;
  while( tab->find_function_list ){
    test_find_function_t *p = tab->find_function_list;
    tab->find_function_list = p->next;
    dbsql_free(p);
  }
  Tcl_DecrRefCount(tab->cmd);
  dbsql_free(tab);
  return DBSQL_OK;
}

/*
** Open a new tcl cursor.
*/
static int __tcl_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  tcl_cursor *cur;
  cur = dbsql_malloc(sizeof(tcl_cursor));
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(tcl_cursor));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Close a tcl cursor.
*/
static int __tcl_close(dbsql_vtab_cursor_t *cur){
  tcl_cursor *p_cur = (tcl_cursor *)cur;
  if( p_cur ){
    dbsql_finalize(p_cur->stmt);
    dbsql_free(p_cur);
  }
  return DBSQL_OK;
}

static int __tcl_next(dbsql_vtab_cursor_t *vtab_cursor){
  tcl_cursor *csr = (tcl_cursor*)vtab_cursor;
  if( csr->stmt ){
    tcl_vtab *tab = (tcl_vtab*)(vtab_cursor->vtab);
    int rc = dbsql_step(csr->stmt);
    if( rc!=DBSQL_ROW ){
      const char *err;
      rc = dbsql_finalize(csr->stmt);
      csr->stmt = 0;
      if( rc!=DBSQL_OK ){
        err = dbsql_errmsg(tab->db);
        tab->base.errmsg = dbsql_mprintf("%s", err);
      }
    }
  }
  return DBSQL_OK;
}

static int __tcl_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  tcl_cursor *csr = (tcl_cursor*)vtab_cursor;
  tcl_vtab *tab = (tcl_vtab*)(vtab_cursor->vtab);
  Tcl_Interp *interp = tab->interp;
  Tcl_Obj *script;
  Tcl_Obj *arg;
  int ii;
  int rc;

  script = Tcl_DuplicateObj(tab->cmd);
  Tcl_IncrRefCount(script);
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj("xFilter", -1));
  Tcl_ListObjAppendElement(interp, script, Tcl_NewIntObj(idx_num));
  Tcl_ListObjAppendElement(
      interp, script, Tcl_NewStringObj(idx_str ? idx_str : "", -1)
  );

  arg = Tcl_NewObj();
  Tcl_IncrRefCount(arg);
  for(ii=0; ii<argc; ii++){
    const char *z_val = (const char*)dbsql_value_text(argv[ii]);
    Tcl_Obj *val;
    if( z_val==0 ){
      dbsql_value_t *mem;
      val = Tcl_NewObj();
      for(rc=dbsql_vtab_in_first(argv[ii], &mem); 
          rc==DBSQL_OK && mem;
          rc=dbsql_vtab_in_next(argv[ii], &mem)
      ){
        Tcl_Obj *val2 = 0;
        z_val = (const char*)dbsql_value_text(mem);
        if( z_val ){
          val2 = Tcl_NewStringObj(z_val, -1);
        }else{
          val2 = Tcl_NewObj();
        }
        Tcl_ListObjAppendElement(interp, val, val2);
      }
    }else{
      val = Tcl_NewStringObj(z_val, -1);
    }
    Tcl_ListObjAppendElement(interp, arg, val);
  }
  Tcl_ListObjAppendElement(interp, script, arg);
  Tcl_DecrRefCount(arg);

  rc = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
  if( rc!=TCL_OK ){
    const char *err = Tcl_GetStringResult(interp);
    rc = DBSQL_ERROR;
    tab->base.errmsg = dbsql_mprintf("%s", err);
  }else{
    /* Analyze the scripts return value. The return value should be a tcl 
    ** list object with an even number of elements. The first element of each
    ** pair must be one of:
    ** 
    **   "sql"          (SQL statement to return data)
    */
    Tcl_Obj *res = Tcl_GetObjResult(interp);
    Tcl_Obj **elems = 0;
    TCL_SIZE n_elem;
    rc = Tcl_ListObjGetElements(interp, res, &n_elem, &elems);
    if( rc!=TCL_OK ){
      const char *err = Tcl_GetStringResult(interp);
      rc = DBSQL_ERROR;
      tab->base.errmsg = dbsql_mprintf("%s", err);
    }else{
      for(ii=0; rc==DBSQL_OK && ii<(int)n_elem; ii+=2){
        const char *cmd = Tcl_GetString(elems[ii]);
        Tcl_Obj *p = elems[ii+1];
        if( dbsql_stricmp("sql", cmd)==0 ){
          const char *sql = Tcl_GetString(p);
          rc = dbsql_prepare_v2(tab->db, sql, -1, &csr->stmt, 0);
          if( rc!=DBSQL_OK ){
            const char *err = dbsql_errmsg(tab->db);
            tab->base.errmsg = dbsql_mprintf("unexpected: %s", err);
          }
        }else{
          rc = DBSQL_ERROR;
          tab->base.errmsg = dbsql_mprintf("unexpected: %s", cmd);
        }
      }
    }
  }

  if( rc==DBSQL_OK ){
    rc = __tcl_next(vtab_cursor);
  }
  return rc;
}

static int __tcl_column(
  dbsql_vtab_cursor_t *vtab_cursor, 
  dbsql_func_t *ctx, 
  int i
){
  tcl_cursor *csr = (tcl_cursor*)vtab_cursor;
  dbsql_result_value(ctx, dbsql_column_value(csr->stmt, i+1));
  return DBSQL_OK;
}

static int __tcl_rowid(dbsql_vtab_cursor_t *vtab_cursor, int64_t *rowid){
  tcl_cursor *csr = (tcl_cursor*)vtab_cursor;
  *rowid = dbsql_column_int64(csr->stmt, 0);
  return DBSQL_OK;
}

static int __tcl_eof(dbsql_vtab_cursor_t *vtab_cursor){
  tcl_cursor *csr = (tcl_cursor*)vtab_cursor;
  return (csr->stmt==0);
}

static void __test_best_index_obj_constraints(
  Tcl_Interp *interp, 
  dbsql_index_info_t *idx_info
){
  int ii;
  Tcl_Obj *res = Tcl_NewObj();
  Tcl_IncrRefCount(res);
  for(ii=0; ii<idx_info->n_constraint; ii++){
    struct dbsql_index_constraint_t const *cons = &idx_info->constraints[ii];
    Tcl_Obj *elem = Tcl_NewObj();
    const char *op = 0;

    Tcl_IncrRefCount(elem);

    switch( cons->op ){
      case DBSQL_INDEX_CONSTRAINT_EQ:
        op = "eq"; break;
      case DBSQL_INDEX_CONSTRAINT_GT:
        op = "gt"; break;
      case DBSQL_INDEX_CONSTRAINT_LE:
        op = "le"; break;
      case DBSQL_INDEX_CONSTRAINT_LT:
        op = "lt"; break;
      case DBSQL_INDEX_CONSTRAINT_GE:
        op = "ge"; break;
      case DBSQL_INDEX_CONSTRAINT_MATCH:
        op = "match"; break;
      case DBSQL_INDEX_CONSTRAINT_LIKE:
        op = "like"; break;
      case DBSQL_INDEX_CONSTRAINT_GLOB:
        op = "glob"; break;
      case DBSQL_INDEX_CONSTRAINT_REGEXP:
        op = "regexp"; break;
      case DBSQL_INDEX_CONSTRAINT_NE:
        op = "ne"; break;
      case DBSQL_INDEX_CONSTRAINT_ISNOT:
        op = "isnot"; break;
      case DBSQL_INDEX_CONSTRAINT_ISNOTNULL:
        op = "isnotnull"; break;
      case DBSQL_INDEX_CONSTRAINT_ISNULL:
        op = "isnull"; break;
      case DBSQL_INDEX_CONSTRAINT_IS:
        op = "is"; break;
      case DBSQL_INDEX_CONSTRAINT_LIMIT:
        op = "limit"; break;
      case DBSQL_INDEX_CONSTRAINT_OFFSET:
        op = "offset"; break;
    }

    Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj("op", -1));
    if( op ){
      Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj(op, -1));
    }else{
      Tcl_ListObjAppendElement(0, elem, Tcl_NewIntObj(cons->op));
    }
    Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj("column", -1));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewIntObj(cons->i_column));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj("usable", -1));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewIntObj(cons->usable));

    Tcl_ListObjAppendElement(0, res, elem);
    Tcl_DecrRefCount(elem);
  }

  Tcl_SetObjResult(interp, res);
  Tcl_DecrRefCount(res);
}

static void __test_best_index_obj_orderby(
  Tcl_Interp *interp, 
  dbsql_index_info_t *idx_info
){
  int ii;
  Tcl_Obj *res = Tcl_NewObj();
  Tcl_IncrRefCount(res);
  for(ii=0; ii<idx_info->n_order_by; ii++){
    struct dbsql_index_orderby_t const *order = &idx_info->order_bies[ii];
    Tcl_Obj *elem = Tcl_NewObj();
    Tcl_IncrRefCount(elem);

    Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj("column", -1));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewIntObj(order->i_column));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewStringObj("desc", -1));
    Tcl_ListObjAppendElement(0, elem, Tcl_NewIntObj(order->desc));

    Tcl_ListObjAppendElement(0, res, elem);
    Tcl_DecrRefCount(elem);
  }

  Tcl_SetObjResult(interp, res);
  Tcl_DecrRefCount(res);
}

/*
** Implementation of the handle passed to each best_index callback. This
** object features the following sub-commands:
**
**    $hdl constraints
**    $hdl orderby
**    $hdl mask
**
**    $hdl distinct
**      Return the result (an integer) of calling dbsql_vtab_distinct()
**      on the index-info structure.
**
**    $hdl in IDX BOOLEAN
**      Wrapper around dbsql_vtab_in(). Returns an integer.
**
**    $hdl rhs_value IDX ?DEFAULT?
**      Wrapper around dbsql_vtab_rhs_value().
*/
static int DBSQL_TCLAPI __test_best_index_obj(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  const char *subs[] = {
    "constraints",                /* 0 */
    "orderby",                    /* 1 */
    "mask",                       /* 2 */
    "distinct",                   /* 3 */
    "in",                         /* 4 */
    "rhs_value",                  /* 5 */
    "collation",                  /* 6 */
    0
  };
  int ii;
  dbsql_index_info_t *idx_info = (dbsql_index_info_t*)clientData;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], subs, "sub-command", 0, &ii) ){
    return TCL_ERROR;
  }

  if( ii<4 && objc!=2 ){
    Tcl_WrongNumArgs(interp, 2, objv, "");
    return TCL_ERROR;
  }
  if( ii==4 && objc!=4 ){
    Tcl_WrongNumArgs(interp, 2, objv, "INDEX BOOLEAN");
    return TCL_ERROR;
  }
  if( ii==5 && objc!=3 && objc!=4 ){
    Tcl_WrongNumArgs(interp, 2, objv, "INDEX ?DEFAULT?");
    return TCL_ERROR;
  }

  switch( ii ){
    case 0: assert( dbsql_stricmp(subs[ii], "constraints")==0 );
      __test_best_index_obj_constraints(interp, idx_info);
      break;

    case 1: assert( dbsql_stricmp(subs[ii], "orderby")==0 );
      __test_best_index_obj_orderby(interp, idx_info);
      break;

    case 2: assert( dbsql_stricmp(subs[ii], "mask")==0 );
      Tcl_SetObjResult(interp, Tcl_NewWideIntObj(idx_info->col_used));
      break;

    case 3: assert( dbsql_stricmp(subs[ii], "distinct")==0 ); {
      int distinct = dbsql_vtab_distinct(idx_info);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(distinct));
      break;
    }

    case 4: assert( dbsql_stricmp(subs[ii], "in")==0 ); {
      int i_cons;
      int handle;
      if( Tcl_GetIntFromObj(interp, objv[2], &i_cons) 
       || Tcl_GetBooleanFromObj(interp, objv[3], &handle) 
      ){
        return TCL_ERROR;
      }
      Tcl_SetObjResult(interp, 
          Tcl_NewIntObj(dbsql_vtab_in(idx_info, i_cons, handle))
      );
      break;
    }

    case 5: assert( dbsql_stricmp(subs[ii], "rhs_value")==0 ); {
      int i_cons = 0;
      int rc;
      dbsql_value_t *val = 0;
      const char *z_val = "";
      if( Tcl_GetIntFromObj(interp, objv[2], &i_cons) ){
        return TCL_ERROR;
      }
      rc = dbsql_vtab_rhs_value(idx_info, i_cons, &val);
      if( rc!=DBSQL_OK && rc!=DBSQL_NOTFOUND ){
        Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
        return TCL_ERROR;
      }
      if( val ){
        z_val = (const char*)dbsql_value_text(val);
      }else if( objc==4 ){
        z_val = Tcl_GetString(objv[3]);
      }
      Tcl_SetObjResult(interp, Tcl_NewStringObj(z_val, -1));
      break;
    }

    case 6: assert( dbsql_stricmp(subs[ii], "collation")==0 ); {
      int i_cons = 0;
      const char *coll = "";
      if( Tcl_GetIntFromObj(interp, objv[2], &i_cons) ){
        return TCL_ERROR;
      }
      coll = dbsql_vtab_collation(idx_info, i_cons);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(coll, -1));
      break;
    }
  }

  return TCL_OK;
}

static int __tcl_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  tcl_vtab *p_tab = (tcl_vtab*)tab;
  Tcl_Interp *interp = p_tab->interp;
  int rc = DBSQL_OK;

  static int i_next = 43;
  char hdl[24];
  Tcl_Obj *script;

  script = Tcl_DuplicateObj(p_tab->cmd);
  Tcl_IncrRefCount(script);
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj("xBestIndex", -1));

  dbsql_snprintf(sizeof(hdl), hdl, "bestindex%d", i_next++);
  Tcl_CreateObjCommand(interp, hdl, __test_best_index_obj, idx_info, 0);
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(hdl, -1));
  rc = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
  Tcl_DeleteCommand(interp, hdl);
  Tcl_DecrRefCount(script);

  if( rc!=TCL_OK ){
    const char *err = Tcl_GetStringResult(interp);
    rc = DBSQL_ERROR;
    p_tab->base.errmsg = dbsql_mprintf("%s", err);
  }else{
    /* Analyze the scripts return value. The return value should be a tcl 
    ** list object with an even number of elements. The first element of each
    ** pair must be one of:
    ** 
    **   "orderby"          (value of order_by_consumed flag)
    **   "cost"             (value of estimated_cost field)
    **   "rows"             (value of estimated_rows field)
    **   "use"              (index of used constraint in constraints[])
    **   "idxnum"           (value of idx_num field)
    **   "idxstr"           (value of idx_str field)
    **   "omit"             (index of omitted constraint in constraints[])
    */
    Tcl_Obj *res = Tcl_GetObjResult(interp);
    Tcl_Obj **elems = 0;
    TCL_SIZE n_elem;
    rc = Tcl_ListObjGetElements(interp, res, &n_elem, &elems);
    if( rc!=TCL_OK ){
      const char *err = Tcl_GetStringResult(interp);
      rc = DBSQL_ERROR;
      p_tab->base.errmsg = dbsql_mprintf("%s", err);
    }else{
      int ii;
      int i_argv = 1;
      for(ii=0; rc==DBSQL_OK && ii<(int)n_elem; ii+=2){
        const char *cmd = Tcl_GetString(elems[ii]);
        Tcl_Obj *p = elems[ii+1];
        if( dbsql_stricmp("cost", cmd)==0 ){
          rc = Tcl_GetDoubleFromObj(interp, p, &idx_info->estimated_cost);
        }else
        if( dbsql_stricmp("orderby", cmd)==0 ){
          rc = Tcl_GetIntFromObj(interp, p, &idx_info->order_by_consumed);
        }else
        if( dbsql_stricmp("idxnum", cmd)==0 ){
          rc = Tcl_GetIntFromObj(interp, p, &idx_info->idx_num);
        }else
        if( dbsql_stricmp("idxstr", cmd)==0 ){
          dbsql_free(idx_info->idx_str);
          idx_info->idx_str = dbsql_mprintf("%s", Tcl_GetString(p));
          idx_info->need_to_free_idx_str = 1;
        }else
        if( dbsql_stricmp("rows", cmd)==0 ){
          Tcl_WideInt x = 0;
          rc = Tcl_GetWideIntFromObj(interp, p, &x);
          idx_info->estimated_rows = (rowcnt_t)x;
        }else
        if( dbsql_stricmp("use", cmd)==0 
         || dbsql_stricmp("omit", cmd)==0 
        ){
          int i_cons;
          rc = Tcl_GetIntFromObj(interp, p, &i_cons);
          if( rc==DBSQL_OK ){
            if( i_cons<0 || i_cons>=idx_info->n_constraint ){
              rc = DBSQL_ERROR;
              p_tab->base.errmsg = dbsql_mprintf("unexpected: %d", i_cons);
            }else{
              int omit = (cmd[0]=='o' || cmd[0]=='O');
              idx_info->constraint_usages[i_cons].argv_index = i_argv++;
              idx_info->constraint_usages[i_cons].omit = omit;
            }
          }
        }else
        if( dbsql_stricmp("constraint", cmd)==0 ){
          rc = DBSQL_CONSTRAINT;
          p_tab->base.errmsg = dbsql_mprintf("%s", Tcl_GetString(p));
        }else{
          rc = DBSQL_ERROR;
          p_tab->base.errmsg = dbsql_mprintf("unexpected: %s", cmd);
        }
        if( rc!=DBSQL_OK && p_tab->base.errmsg==0 ){
          const char *err = Tcl_GetStringResult(interp);
          p_tab->base.errmsg = dbsql_mprintf("%s", err);
        }
      }
    }
  }

  return rc;
}

static void __tcl_function(dbsql_func_t *ctx, int n_arg, dbsql_value_t **args){
  test_find_function_t *p = (test_find_function_t*)dbsql_user_data(ctx);
  Tcl_Interp *interp = p->tab->interp;
  Tcl_Obj *script = 0;
  Tcl_Obj *ret = 0;
  int ii;

  script = Tcl_DuplicateObj(p->tab->cmd);
  Tcl_IncrRefCount(script);
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj("function", -1));
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(p->name, -1));

  for(ii=0; ii<n_arg; ii++){
    const char *arg = (const char*)dbsql_value_text(args[ii]);
    Tcl_ListObjAppendElement(interp, script,
        (arg ? Tcl_NewStringObj(arg, -1) : Tcl_NewObj())
    );
  }
  Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
  Tcl_DecrRefCount(script);

  ret = Tcl_GetObjResult(interp);
  dbsql_result_text(ctx, Tcl_GetString(ret), -1, DBSQL_TRANSIENT);
}

static int __tcl_find_function(
  dbsql_vtab_t *tab, 
  int n_arg, 
  const char *name,
  void (**px_func)(dbsql_func_t*,int,dbsql_value_t**),   /* OUT */
  void **arg                                             /* OUT */
){
  int i_ret = 0;
  tcl_vtab *p_tab = (tcl_vtab*)tab;
  Tcl_Interp *interp = p_tab->interp;
  Tcl_Obj *script = 0;
  int rc = DBSQL_OK;

  script = Tcl_DuplicateObj(p_tab->cmd);
  Tcl_IncrRefCount(script);
  Tcl_ListObjAppendElement(
      interp, script, Tcl_NewStringObj("xFindFunction", -1)
  );
  Tcl_ListObjAppendElement(interp, script, Tcl_NewIntObj(n_arg));
  Tcl_ListObjAppendElement(interp, script, Tcl_NewStringObj(name, -1));
  rc = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
  Tcl_DecrRefCount(script);

  if( rc==DBSQL_OK ){
    Tcl_Obj *obj = Tcl_GetObjResult(interp);

    if( Tcl_GetIntFromObj(interp, obj, &i_ret) ){
      rc = DBSQL_ERROR;
    }else if( i_ret>0 ){
      int64_t n_name = strlen(name);
      int64_t n_byte = n_name + 1 + sizeof(test_find_function_t);
      test_find_function_t *p_new = 0;

      p_new = (test_find_function_t*)dbsql_malloc64(n_byte);
      if( p_new==0 ){
        i_ret = 0;
      }else{
        memset(p_new, 0, n_byte);
        p_new->name = (const char*)&p_new[1];
        memcpy((char*)p_new->name, name, n_name);
        p_new->tab = p_tab;
        p_new->next = p_tab->find_function_list;
        p_tab->find_function_list = p_new;
        *arg = (void*)p_new;
        *px_func = __tcl_function;
      }
    }
  }

  return i_ret;
}

static int __tcl_update(
  dbsql_vtab_t *tab, 
  int n_arg, 
  dbsql_value_t **vals, 
  int64_t *pi_rowid
){
  tcl_vtab *p_tab = (tcl_vtab*)tab;
  Tcl_Interp *interp = p_tab->interp; 
  Tcl_Obj *eval = Tcl_DuplicateObj(p_tab->cmd);
  int rc = TCL_OK;

  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(interp, eval, Tcl_NewStringObj("xUpdate",-1));

  rc = Tcl_EvalObjEx(interp, eval, TCL_EVAL_GLOBAL);
  Tcl_DecrRefCount(eval);

  if( rc==TCL_OK ){
    Tcl_Obj *res = Tcl_GetObjResult(interp);
    Tcl_WideInt v;
    rc = Tcl_GetWideIntFromObj(interp, res, &v);
    *pi_rowid = (int64_t)v;
  }

  if( rc!=TCL_OK ){
    tab->errmsg = dbsql_mprintf("%s", Tcl_GetStringResult(p_tab->interp));
    return rc;
  }

  return DBSQL_OK;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static dbsql_module_t __tcl_module = {
  0,                         /* i_version */
  __tcl_connect,
  __tcl_connect,
  __tcl_best_index,
  __tcl_disconnect, 
  __tcl_disconnect,
  __tcl_open,                      /* x_open - open a cursor */
  __tcl_close,                     /* x_close - close a cursor */
  __tcl_filter,                    /* filter - configure scan constraints */
  __tcl_next,                      /* next - advance a cursor */
  __tcl_eof,                       /* eof - check for end of scan */
  __tcl_column,                    /* column - read data */
  __tcl_rowid,                     /* rowid - read data */
  0,                           /* update */
  0,                           /* begin */
  0,                           /* x_sync */
  0,                           /* commit */
  0,                           /* rollback */
  __tcl_find_function,             /* find_function */
  0,                           /* rename */
  0,                           /* savepoint */
  0,                           /* release */
  0,                           /* rollback_to */
  0,                           /* shadow_name */
  0                            /* integrity */
};
static dbsql_module_t __tcl_module_update = {
  0,                         /* i_version */
  __tcl_connect,
  __tcl_connect,
  __tcl_best_index,
  __tcl_disconnect, 
  __tcl_disconnect,
  __tcl_open,                      /* x_open - open a cursor */
  __tcl_close,                     /* x_close - close a cursor */
  __tcl_filter,                    /* filter - configure scan constraints */
  __tcl_next,                      /* next - advance a cursor */
  __tcl_eof,                       /* eof - check for end of scan */
  __tcl_column,                    /* column - read data */
  __tcl_rowid,                     /* rowid - read data */
  __tcl_update,                   /* update */
  0,                           /* begin */
  0,                           /* x_sync */
  0,                           /* commit */
  0,                           /* rollback */
  __tcl_find_function,             /* find_function */
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

static void __del_test_vtab_ctx(void *p){
  test_vtab_context_t *ctx = (test_vtab_context_t*)p;
  if( ctx->p_default ){
    Tcl_DecrRefCount(ctx->p_default);
  }
  ckfree(ctx);
}

/*
** Register the echo virtual table module.
*/
static int DBSQL_TCLAPI register_tcl_module(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  DBSQL *db;
  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB ?DEFAULT-CMD?");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  {
    dbsql_module_t *mod = &__tcl_module;
    test_vtab_context_t *ctx = (test_vtab_context_t*)ckalloc(sizeof(test_vtab_context_t));
    ctx->interp = interp;
    ctx->p_default = 0;
    if( objc==3 ){
      ctx->p_default = objv[2];
      Tcl_IncrRefCount(ctx->p_default);
    }

    if( objc==3 ){ mod = &__tcl_module_update; }
    dbsql_create_module_v2(db, "tcl", mod, (void*)ctx, __del_test_vtab_ctx);
  }
#endif
  return TCL_OK;
}

#endif


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltesttcl_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "register_tcl_module",   register_tcl_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
