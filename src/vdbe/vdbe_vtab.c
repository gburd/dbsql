/*
** 2020-03-23
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
** This file implements virtual-tables for examining the bytecode content
** of a prepared statement.
*/
#include "dbsql_int.h"
#if defined(DBSQL_ENABLE_BYTECODE_VTAB) && !defined(DBSQL_OMIT_VIRTUALTABLE)
#include "vdbe_int.h"

/* An instance of the bytecode() table-valued function.
*/
typedef struct bytecodevtab bytecodevtab;
struct bytecodevtab {
  dbsql_vtab_t base;     /* Base class - must be first */
  DBSQL *db;           /* Database connection */
  int tables_used;       /* 2 for tables_used().  0 for bytecode(). */
};

/* A cursor for scanning through the bytecode
*/
typedef struct bytecodevtab_cursor bytecodevtab_cursor;
struct bytecodevtab_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  dbsql_stmt_t *stmt;       /* The statement whose bytecode is displayed */
  int i_rowid;                /* The rowid of the output table */
  int i_addr;                 /* Address */
  int need_finalize;          /* Cursors owns stmt and must finalize it */
  int show_subprograms;       /* Provide a listing of subprograms */
  op_t *ops;                   /* Operand array */
  char *p4;                 /* Rendered P4 value */
  const char *type;         /* tables_used.type */
  const char *schema;       /* tables_used.schema */
  const char *name;         /* tables_used.name */
  mem_t sub;                   /* Subprograms */
};

/*
** Create a new bytecode() table-valued function.
*/
static int __bytecodevtab_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  bytecodevtab *p_new;
  int rc;
  int is_tab_used = aux!=0;
  const char *schemas[2] = {
    /* bytecode() schema */
    "CREATE TABLE x("
      "addr INT,"
      "opcode TEXT,"
      "p1 INT,"
      "p2 INT,"
      "p3 INT,"
      "p4 TEXT,"
      "p5 INT,"
      "comment TEXT,"
      "subprog TEXT," 
      "nexec INT,"
      "ncycle INT,"
      "stmt HIDDEN"
    ");",

    /* Tables_used() schema */
    "CREATE TABLE x("
      "type TEXT,"
      "schema TEXT,"
      "name TEXT,"
      "wr INT,"
      "subprog TEXT," 
      "stmt HIDDEN"
   ");"
  };

  (void)argc;
  (void)argv;
  (void)err_p;
  rc = dbsql_declare_vtab(db, schemas[is_tab_used]);
  if( rc==DBSQL_OK ){
    p_new = dbsql_malloc( sizeof(*p_new) );
    *vtab = (dbsql_vtab_t*)p_new;
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
    p_new->db = db;
    p_new->tables_used = is_tab_used*2;
  }
  return rc;
}

/*
** This method is the destructor for bytecodevtab objects.
*/
static int __bytecodevtab_disconnect(dbsql_vtab_t *vtab){
  bytecodevtab *p = (bytecodevtab*)vtab;
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Constructor for a new bytecodevtab_cursor object.
*/
static int __bytecodevtab_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  bytecodevtab *v_tab = (bytecodevtab*)p;
  bytecodevtab_cursor *cur;
  cur = dbsql_malloc( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  __vdbe_mem_init(&cur->sub, v_tab->db, 1);
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Clear all internal content from a bytecodevtab cursor.
*/
static void __bytecodevtab_cursor_clear(bytecodevtab_cursor *cur){
  dbsql_free(cur->p4);
  cur->p4 = 0;
  __vdbe_mem_release(&cur->sub);
  __vdbe_mem_set_null(&cur->sub);
  if( cur->need_finalize ){
    dbsql_finalize(cur->stmt);
  }
  cur->stmt = 0;
  cur->need_finalize = 0;
  cur->type = 0;
  cur->schema = 0;
  cur->name = 0;
}

/*
** Destructor for a bytecodevtab_cursor.
*/
static int __bytecodevtab_close(dbsql_vtab_cursor_t *cur){
  bytecodevtab_cursor *p_cur = (bytecodevtab_cursor*)cur;
  __bytecodevtab_cursor_clear(p_cur);
  dbsql_free(p_cur);
  return DBSQL_OK;
}


/*
** Advance a bytecodevtab_cursor to its next row of output.
*/
static int __bytecodevtab_next(dbsql_vtab_cursor_t *cur){
  bytecodevtab_cursor *p_cur = (bytecodevtab_cursor*)cur;
  bytecodevtab *tab = (bytecodevtab*)cur->vtab;
  int rc;
  if( p_cur->p4 ){
    dbsql_free(p_cur->p4);
    p_cur->p4 = 0;
  }
  if( p_cur->name ){
    p_cur->name = 0;
    p_cur->type = 0;
    p_cur->schema = 0;
  }
  rc = __vdbe_next_opcode(
           (vdbe_t*)p_cur->stmt, 
           p_cur->show_subprograms ? &p_cur->sub : 0,
           tab->tables_used,
           &p_cur->i_rowid,
           &p_cur->i_addr,
           &p_cur->ops);
  if( rc!=DBSQL_OK ){
    __vdbe_mem_set_null(&p_cur->sub);
    p_cur->ops = 0;
  }
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __bytecodevtab_eof(dbsql_vtab_cursor_t *cur){
  bytecodevtab_cursor *p_cur = (bytecodevtab_cursor*)cur;
  return p_cur->ops==0;
}

/*
** Return values of columns for the row at which the bytecodevtab_cursor
** is currently pointing.
*/
static int __bytecodevtab_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  bytecodevtab_cursor *p_cur = (bytecodevtab_cursor*)cur;
  bytecodevtab *v_tab = (bytecodevtab*)cur->vtab;
  op_t *op = p_cur->ops + p_cur->i_addr;
  if( v_tab->tables_used ){
    if( i==4 ){
      i = 8;
    }else{
      if( i<=2 && p_cur->type==0 ){
        schema_t *schema;
        hash_elem_t *k;
        int i_db = op->p3;
        pgno_t i_root = (pgno_t)op->p2;
        DBSQL *db = v_tab->db;
        schema = db->dbs[i_db].schema;
        p_cur->schema = db->dbs[i_db].db_s_name;
        for(k=dbsql_hash_first(&schema->tbl_hash); k; k=dbsql_hash_next(k)){
          table_t *tab = (table_t*)dbsql_hash_data(k);
          if( !IS_VIRTUAL(tab) && tab->tnum==i_root ){
            p_cur->name = tab->name;
            p_cur->type = "table";
            break;
          }
        }
        if( p_cur->name==0 ){
          for(k=dbsql_hash_first(&schema->idx_hash); k; k=dbsql_hash_next(k)){
            index_t *idx = (index_t*)dbsql_hash_data(k);
            if( idx->tnum==i_root ){
              p_cur->name = idx->name;
              p_cur->type = "index";
            }
          }
        }
      }
      i += 20;
    }
  }
  switch( i ){
    case 0:   /* addr */
      dbsql_result_int(ctx, p_cur->i_addr);
      break;
    case 1:   /* opcode */
      dbsql_result_text(ctx, (char*)__opcode_name(op->opcode),
                          -1, DBSQL_STATIC);
      break;
    case 2:   /* p1 */
      dbsql_result_int(ctx, op->p1);
      break;
    case 3:   /* p2 */
      dbsql_result_int(ctx, op->p2);
      break;
    case 4:   /* p3 */
      dbsql_result_int(ctx, op->p3);
      break;
    case 5:   /* p4 */
    case 7:   /* comment */
      if( p_cur->p4==0 ){
        p_cur->p4 = __vdbe_display_p4(v_tab->db, op);
      }
      if( i==5 ){
        dbsql_result_text(ctx, p_cur->p4, -1, DBSQL_STATIC);
      }else{
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
        char *com = __vdbe_display_comment(v_tab->db, op, p_cur->p4);
        dbsql_result_text(ctx, com, -1, dbsql_free);
#endif
      }
      break;
    case 6:     /* p5 */
      dbsql_result_int(ctx, op->p5);
      break;
    case 8: {   /* subprog */
      op_t *ops = p_cur->ops;
      assert( ops[0].opcode==OP_Init );
      assert( ops[0].p4.z==0 || strncmp(ops[0].p4.z,"-" "- ",3)==0 );
      if( p_cur->i_rowid==p_cur->i_addr+1 ){
        break;  /* Result is NULL for the main program */
      }else if( ops[0].p4.z!=0 ){
         dbsql_result_text(ctx, ops[0].p4.z+3, -1, DBSQL_STATIC);
      }else{
         dbsql_result_text(ctx, "(FK)", 4, DBSQL_STATIC);
      }
      break;
    }

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
    case 9:     /* nexec */
      dbsql_result_int64(ctx, op->n_exec);
      break;
    case 10:    /* ncycle */
      dbsql_result_int64(ctx, op->n_cycle);
      break;
#else
    case 9:     /* nexec */
    case 10:    /* ncycle */
      dbsql_result_int(ctx, 0);
      break;
#endif

    case 20:  /* tables_used.type */
      dbsql_result_text(ctx, p_cur->type, -1, DBSQL_STATIC);
      break;
    case 21:  /* tables_used.schema */
      dbsql_result_text(ctx, p_cur->schema, -1, DBSQL_STATIC);
      break;
    case 22:  /* tables_used.name */
      dbsql_result_text(ctx, p_cur->name, -1, DBSQL_STATIC);
      break;
    case 23:  /* tables_used.wr */
      dbsql_result_int(ctx, op->opcode==OP_OpenWrite);
      break;
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int __bytecodevtab_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  bytecodevtab_cursor *p_cur = (bytecodevtab_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Initialize a cursor.
**
**    idx_num==0     means show all subprograms
**    idx_num==1     means show only the main bytecode and omit subprograms.
*/
static int __bytecodevtab_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  bytecodevtab_cursor *cur = (bytecodevtab_cursor *)vtab_cursor;
  bytecodevtab *v_tab = (bytecodevtab *)vtab_cursor->vtab;
  int rc = DBSQL_OK;
  (void)idx_str;

  __bytecodevtab_cursor_clear(cur);
  cur->i_rowid = 0;
  cur->i_addr = 0;
  cur->show_subprograms = idx_num==0;
  assert( argc==1 );
  if( dbsql_value_type(argv[0])==DBSQL_TEXT ){
    const char *sql = (const char*)dbsql_value_text(argv[0]);
    if( sql==0 ){
      rc = DBSQL_NOMEM;
    }else{
      rc = dbsql_prepare_v2(v_tab->db, sql, -1, &cur->stmt, 0);
      cur->need_finalize = 1;
    }
  }else{
    cur->stmt = (dbsql_stmt_t*)dbsql_value_pointer(argv[0],"stmt-pointer");
  }
  if( cur->stmt==0 ){
    v_tab->base.errmsg = dbsql_mprintf(
       "argument to %s() is not a valid SQL statement",
       v_tab->tables_used ? "tables_used" : "bytecode"
    );
    rc = DBSQL_ERROR;
  }else{
    __bytecodevtab_next(vtab_cursor);
  }
  return rc;
}

/*
** We must have a single stmt=? constraint that will be passed through
** into the filter method.  If there is no valid stmt=? constraint,
** then return an DBSQL_CONSTRAINT error.
*/
static int __bytecodevtab_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;
  int rc = DBSQL_CONSTRAINT;
  struct dbsql_index_constraint_t *p;
  bytecodevtab *v_tab = (bytecodevtab*)tab;
  int i_base_col = v_tab->tables_used ? 4 : 10;
  idx_info->estimated_cost = (double)100;
  idx_info->estimated_rows = 100;
  idx_info->idx_num = 0;
  for(i=0, p=idx_info->constraints; i<idx_info->n_constraint; i++, p++){
    if( p->usable==0 ) continue;
    if( p->op==DBSQL_INDEX_CONSTRAINT_EQ && p->i_column==i_base_col+1 ){
      rc = DBSQL_OK;
      idx_info->constraint_usages[i].omit = 1;
      idx_info->constraint_usages[i].argv_index = 1;
    }
    if( p->op==DBSQL_INDEX_CONSTRAINT_ISNULL && p->i_column==i_base_col ){
      idx_info->constraint_usages[i].omit = 1;
      idx_info->idx_num = 1;
    }
  }
  return rc;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static dbsql_module_t __bytecodevtab_module = {
  /* i_version    */ 0,
  /* create     */ 0,
  /* connect    */ __bytecodevtab_connect,
  /* best_index  */ __bytecodevtab_best_index,
  /* disconnect */ __bytecodevtab_disconnect,
  /* destroy    */ 0,
  /* x_open       */ __bytecodevtab_open,
  /* x_close      */ __bytecodevtab_close,
  /* filter     */ __bytecodevtab_filter,
  /* next       */ __bytecodevtab_next,
  /* eof        */ __bytecodevtab_eof,
  /* column     */ __bytecodevtab_column,
  /* rowid      */ __bytecodevtab_rowid,
  /* update     */ 0,
  /* begin      */ 0,
  /* x_sync       */ 0,
  /* commit     */ 0,
  /* rollback   */ 0,
  /* find_method */ 0,
  /* rename     */ 0,
  /* savepoint  */ 0,
  /* release    */ 0,
  /* rollback_to */ 0,
  /* shadow_name */ 0,
  /* integrity  */ 0
};


int __vdbe_bytecode_vtab_init(DBSQL *db){
  int rc;
  rc = dbsql_create_module(db, "bytecode", &__bytecodevtab_module, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_module(db, "tables_used", &__bytecodevtab_module, &db);
  }
  return rc;
}
#elif defined(DBSQL_ENABLE_BYTECODE_VTAB)
int __vdbe_bytecode_vtab_init(DBSQL *db){ return DBSQL_OK; }
#endif /* DBSQL_ENABLE_BYTECODE_VTAB */
