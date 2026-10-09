/*
** 2005 February 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that used to generate VDBE code
** that implements the ALTER TABLE command.
*/
#include "dbsql_int.h"

/*
** The code in this file only exists if we are not omitting the
** ALTER TABLE logic from the build.
*/
#ifndef DBSQL_OMIT_ALTERTABLE

/*
** Parameter name is the name of a table that is about to be altered
** (either with ALTER TABLE ... RENAME TO or ALTER TABLE ... ADD COLUMN).
** If the table is a system table, this function leaves an error message
** in parser->err (system tables may not be altered) and returns non-zero.
**
** Or, if name is not a system table, zero is returned.
*/
static int __is_alterable_table(parser_t *parser, table_t *tab){
  if( 0==__str_nicmp(tab->name, "dbsql_", 6)
#ifndef DBSQL_OMIT_VIRTUALTABLE
   || (tab->tab_flags & TF_Eponymous)!=0
   || ( (tab->tab_flags & TF_Shadow)!=0
        && __read_only_shadow_tables(parser->db)
   )
#endif
  ){
    __error_msg(parser, "table %s may not be altered", tab->name);
    return 1;
  }
  return 0;
}

/*
** Generate code to verify that the schemas of database db and, if
** temp is not true, database "temp", can still be parsed. This is
** called at the end of the generation of an ALTER TABLE ... RENAME ...
** statement to ensure that the operation has not rendered any schema
** objects unusable.
*/
static void __rename_test_schema(
  parser_t *parser,                  /* parser_t context */
  const char *db,                /* Name of db to verify schema of */
  int temp,                      /* True if this is the temp db */
  const char *when,              /* "when" part of error message */
  int no_dqs                      /* Do not allow DQS in the schema */
){
  parser->col_names_set = 1;
  __nested_parse(parser,
      "SELECT 1 "
      "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
      "WHERE name NOT LIKE 'dbsqlX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'"
      " AND dbsql_rename_test(%Q, sql, type, name, %d, %Q, %d)=NULL ",
      db,
      db, temp, when, no_dqs
  );

  if( temp==0 ){
    __nested_parse(parser,
        "SELECT 1 "
        "FROM temp." LEGACY_SCHEMA_TABLE " "
        "WHERE name NOT LIKE 'dbsqlX_%%' ESCAPE 'X'"
        " AND sql NOT LIKE 'create virtual%%'"
        " AND dbsql_rename_test(%Q, sql, type, name, 1, %Q, %d)=NULL ",
        db, when, no_dqs
    );
  }
}

/*
** Generate VM code to replace any double-quoted strings (but not double-quoted
** identifiers) within the "sql" column of the dbsql_schema table in
** database db with their single-quoted equivalents. If argument temp is
** not true, similarly update all SQL statements in the dbsql_schema table
** of the temp db.
*/
static void __rename_fix_quotes(parser_t *parser, const char *db, int temp){
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE
      " SET sql = dbsql_rename_quotefix(%Q, sql)"
      "WHERE name NOT LIKE 'dbsqlX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'" , db, db
  );
  if( temp==0 ){
    __nested_parse(parser,
      "UPDATE temp." LEGACY_SCHEMA_TABLE
      " SET sql = dbsql_rename_quotefix('temp', sql)"
      "WHERE name NOT LIKE 'dbsqlX_%%' ESCAPE 'X'"
      " AND sql NOT LIKE 'create virtual%%'"
    );
  }
}

/*
** Generate code to reload the schema for database i_db. And, if i_db!=1, for
** the temp database as well.
*/
static void __rename_reload_schema(parser_t *parser, int i_db, u_int16_t p5){
  vdbe_t *v = parser->vdbe;
  if( v ){
    __change_cookie(parser, i_db);
    __vdbe_add_parse_schema_op(parser->vdbe, i_db, 0, p5);
    if( i_db!=1 ) __vdbe_add_parse_schema_op(parser->vdbe, 1, 0, p5);
  }
}

/*
** Generate code to implement the "ALTER TABLE xxx RENAME TO yyy"
** command.
*/
void __alter_rename_table(
  parser_t *parser,            /* Parser context. */
  src_list_t *src,            /* The table to rename. */
  Token *name              /* The new table name. */
){
  int i_db;                  /* Database that contains the table */
  char *z_db;                /* Name of database i_db */
  table_t *tab;              /* table_t being renamed */
  char *z_name = 0;          /* NULL-terminated version of name */
  DBSQL *db = parser->db; /* Database connection */
  int n_tab_name;             /* Number of UTF-8 characters in tab_name */
  const char *tab_name;     /* Original name of the table */
  vdbe_t *v;
  v_table_t *v_tab = 0;        /* Non-zero if this is a v-tab with an rename() */

  if( NEVER(db->malloc_failed) ) goto exit_rename_table;
  assert( src->n_src==1 );
  assert( __sm_holds_all_mutexes(parser->db) );

  tab = __locate_table_item(parser, 0, &src->a[0]);
  if( !tab ) goto exit_rename_table;
  i_db = __schema_to_index(parser->db, tab->schema);
  z_db = db->dbs[i_db].db_s_name;

  /* Get a NULL terminated version of the new table name. */
  z_name = __name_from_token(db, name);
  if( !z_name ) goto exit_rename_table;

  /* Check that a table or index named 'name' does not already exist
  ** in database i_db. If so, this is an error.
  */
  if( __find_table(db, z_name, z_db)
   || __find_index(db, z_name, z_db)
   || __is_shadow_table_of(db, tab, z_name)
  ){
    __error_msg(parser,
        "there is already another table or index with this name: %s", z_name);
    goto exit_rename_table;
  }

  /* Make sure it is not a system table being altered, or a reserved name
  ** that the table is being renamed to.
  */
  if( DBSQL_OK!=__is_alterable_table(parser, tab) ){
    goto exit_rename_table;
  }
  if( DBSQL_OK!=__check_object_name(parser,z_name,"table",z_name) ){
    goto exit_rename_table;
  }

#ifndef DBSQL_OMIT_VIEW
  if( IsView(tab) ){
    __error_msg(parser, "view %s may not be altered", tab->name);
    goto exit_rename_table;
  }
#endif

#ifndef DBSQL_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( __auth_check(parser, DBSQL_ALTER_TABLE, z_db, tab->name, 0) ){
    goto exit_rename_table;
  }
#endif

#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( __view_get_column_names(parser, tab) ){
    goto exit_rename_table;
  }
  if( IS_VIRTUAL(tab) ){
    v_tab = __get_v_table(db, tab);
    if( v_tab->vtab->module->rename==0 ){
      v_tab = 0;
    }
  }
#endif

  /* Begin a transaction for database i_db. Then modify the schema cookie
  ** (since the ALTER TABLE modifies the schema). Call __may_abort(),
  ** as the scalar functions (e.g. dbsql_rename_table()) invoked by the
  ** nested SQL may raise an exception.  */
  v = __get_vdbe(parser);
  if( v==0 ){
    goto exit_rename_table;
  }
  __may_abort(parser);

  /* figure out how many UTF-8 characters are in name */
  tab_name = tab->name;
  n_tab_name = __utf8_char_len(tab_name, -1);

  /* Rewrite all CREATE TABLE, INDEX, TRIGGER or VIEW statements in
  ** the schema to use the new table name.  */
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_rename_table(%Q, type, name, sql, %Q, %Q, %d) "
      "WHERE (type!='index' OR tbl_name=%Q COLLATE nocase)"
      "AND   name NOT LIKE 'dbsqlX_%%' ESCAPE 'X'"
      , z_db, z_db, tab_name, z_name, (i_db==1), tab_name
  );

  /* Update the tbl_name and name columns of the dbsql_schema table
  ** as required.  */
  __nested_parse(parser,
      "UPDATE %Q." LEGACY_SCHEMA_TABLE " SET "
          "tbl_name = %Q, "
          "name = CASE "
            "WHEN type='table' THEN %Q "
            "WHEN name LIKE 'dbsqlX_autoindex%%' ESCAPE 'X' "
            "     AND type='index' THEN "
             "'dbsql_autoindex_' || %Q || substr(name,%d+17) "
            "ELSE name END "
      "WHERE tbl_name=%Q COLLATE nocase AND "
          "(type='table' OR type='index' OR type='trigger');",
      z_db,
      z_name, z_name, z_name,
      n_tab_name, tab_name
  );

#ifndef DBSQL_OMIT_AUTOINCREMENT
  /* If the dbsql_sequence table exists in this database, then update
  ** it with the new table name.
  */
  if( __find_table(db, "dbsql_sequence", z_db) ){
    __nested_parse(parser,
        "UPDATE \"%w\".dbsql_sequence set name = %Q WHERE name = %Q",
        z_db, z_name, tab->name);
  }
#endif

  /* If the table being renamed is not itself part of the temp database,
  ** edit view and trigger definitions within the temp database
  ** as required.  */
  if( i_db!=1 ){
    __nested_parse(parser,
        "UPDATE dbsql_temp_schema SET "
            "sql = dbsql_rename_table(%Q, type, name, sql, %Q, %Q, 1), "
            "tbl_name = "
              "CASE WHEN tbl_name=%Q COLLATE nocase AND "
              "  dbsql_rename_test(%Q, sql, type, name, 1, 'after rename', 0) "
              "THEN %Q ELSE tbl_name END "
            "WHERE type IN ('view', 'trigger')"
        , z_db, tab_name, z_name, tab_name, z_db, z_name);
  }

  /* If this is a virtual table, invoke the rename() function if
  ** one is defined. The rename() callback will modify the names
  ** of any resources used by the v-table implementation (including other
  ** dbsql tables) that are identified by the name of the virtual table.
  */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( v_tab ){
    int i = ++parser->n_mem;
    __vdbe_load_string(v, i, z_name);
    __vdbe_add_op4(v, OP_VRename, i, 0, 0,(const char*)v_tab, P4_VTAB);
  }
#endif

  __rename_reload_schema(parser, i_db, INITFLAG_AlterRename);
  __rename_test_schema(parser, z_db, i_db==1, "after rename", 0);

exit_rename_table:
  __src_list_delete(db, src);
  __dbsql_db_free(db, z_name);
}

/*
** Write code that will raise an error if the table described by
** db and tab is not empty.
*/
static void __error_if_not_empty(
  parser_t *parser,        /* Parsing context */
  const char *db,      /* schema_t holding the table */
  const char *tab,     /* table_t to check for empty */
  const char *err      /* Error message text */
){
  __nested_parse(parser,
     "SELECT raise(ABORT,%Q) FROM \"%w\".\"%w\"",
     err, db, tab
  );
}

/*
** This function is called after an "ALTER TABLE ... ADD" statement
** has been parsed. Argument col_def contains the text of the new
** column definition.
**
** The table_t structure parser->new_table was extended to include
** the new column during parsing.
*/
void __alter_finish_add_column(parser_t *parser, Token *col_def){
  table_t *p_new;              /* Copy of parser->new_table */
  table_t *tab;              /* table_t being altered */
  int i_db;                  /* Database number */
  const char *z_db;          /* Database name */
  const char *z_tab;         /* table_t name */
  char *z_col;               /* Null-terminated column definition */
  column_t *col;             /* The new column */
  expr_t *dflt;              /* Default value for the new column */
  DBSQL *db;              /* The database connection; */
  vdbe_t *v;                  /* The prepared statement under construction */
  int r1;                   /* Temporary registers */

  db = parser->db;
  assert( db->parser==parser );
  if( parser->n_err ) return;
  assert( db->malloc_failed==0 );
  p_new = parser->new_table;
  assert( p_new );

  assert( __sm_holds_all_mutexes(db) );
  i_db = __schema_to_index(db, p_new->schema);
  z_db = db->dbs[i_db].db_s_name;
  z_tab = &p_new->name[15];  /* Skip the "dbsql_altertab_" prefix on the name */
  col = &p_new->cols[p_new->n_col-1];
  dflt = __column_expr(p_new, col);
  tab = __find_table(db, z_tab, z_db);
  assert( tab );

#ifndef DBSQL_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( __auth_check(parser, DBSQL_ALTER_TABLE, z_db, tab->name, 0) ){
    return;
  }
#endif


  /* Check that the new column is not specified as PRIMARY KEY or UNIQUE.
  ** If there is a NOT NULL constraint, then the default value for the
  ** column must not be NULL.
  */
  if( col->col_flags & COLFLAG_PRIMKEY ){
    __error_msg(parser, "Cannot add a PRIMARY KEY column");
    return;
  }
  if( p_new->p_index ){
    __error_msg(parser,
         "Cannot add a UNIQUE column");
    return;
  }
  if( (col->col_flags & COLFLAG_GENERATED)==0 ){
    /* If the default value for the new column was specified with a
    ** literal NULL, then set dflt to 0. This simplifies checking
    ** for an SQL NULL default below.
    */
    assert( dflt==0 || dflt->op==TK_SPAN );
    if( dflt && dflt->left->op==TK_NULL ){
      dflt = 0;
    }
    assert( IS_ORDINARY_TABLE(p_new) );
    if( (db->flags&DBSQL_ForeignKeys) && p_new->u.tab.f_key && dflt ){
      __error_if_not_empty(parser, z_db, z_tab,
          "Cannot add a REFERENCES column with non-NULL default value");
    }
    if( col->not_null && !dflt ){
      __error_if_not_empty(parser, z_db, z_tab,
          "Cannot add a NOT NULL column with default value NULL");
    }


    /* Ensure the default expression is something that __value_from_expr()
    ** can handle (i.e. not CURRENT_TIME etc.)
    */
    if( dflt ){
      dbsql_value_t *val = 0;
      int rc;
      rc = __value_from_expr(db, dflt, DBSQL_UTF8, DBSQL_AFF_BLOB, &val);
      assert( rc==DBSQL_OK || rc==DBSQL_NOMEM );
      if( rc!=DBSQL_OK ){
        assert( db->malloc_failed == 1 );
        return;
      }
      if( !val ){
        __error_if_not_empty(parser, z_db, z_tab,
           "Cannot add a column with non-constant default");
      }
      __value_free(val);
    }
  }else if( col->col_flags & COLFLAG_STORED ){
    __error_if_not_empty(parser, z_db, z_tab, "cannot add a STORED column");
  }


  /* Modify the CREATE TABLE statement. */
  z_col = __db_str_n_dup(db, (char*)col_def->z, col_def->n);
  if( z_col ){
    char *end = &z_col[col_def->n-1];
    while( end>z_col && (*end==';' || __isspace(*end)) ){
      *end-- = '\0';
    }
    /* substr() operations on characters, but add_col_offset is in bytes. So we
    ** have to use printf() to translate between these units: */
    assert( IS_ORDINARY_TABLE(tab) );
    assert( IS_ORDINARY_TABLE(p_new) );
    __nested_parse(parser,
        "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
          "sql = printf('%%.%ds, ',sql) || %Q"
          " || substr(sql,1+length(printf('%%.%ds',sql))) "
        "WHERE type = 'table' AND name = %Q",
      z_db, p_new->u.tab.add_col_offset, z_col, p_new->u.tab.add_col_offset,
      z_tab
    );
    __dbsql_db_free(db, z_col);
  }

  v = __get_vdbe(parser);
  if( v ){
    /* Make sure the schema version is at least 3.  But do not upgrade
    ** from less than 3 to 4, as that will corrupt any preexisting DESC
    ** index.
    */
    r1 = __get_temp_reg(parser);
    __vdbe_add_op3(v, OP_ReadCookie, i_db, r1, BTREE_FILE_FORMAT);
    __vdbe_uses_btree(v, i_db);
    __vdbe_add_op2(v, OP_AddImm, r1, -2);
    __vdbe_add_op2(v, OP_IfPos, r1, __vdbe_current_addr(v)+2);
    VDBE_COVERAGE(v);
    __vdbe_add_op3(v, OP_SetCookie, i_db, BTREE_FILE_FORMAT, 3);
    __release_temp_reg(parser, r1);

    /* Reload the table definition */
    __rename_reload_schema(parser, i_db, INITFLAG_AlterAdd);

    /* Verify that constraints are still satisfied */
    if( p_new->check!=0
     || (col->not_null && (col->col_flags & COLFLAG_GENERATED)!=0)
     || (tab->tab_flags & TF_Strict)!=0
    ){
      __nested_parse(parser,
        "SELECT CASE WHEN quick_check GLOB 'CHECK*'"
        " THEN raise(ABORT,'CHECK constraint failed')"
        " WHEN quick_check GLOB 'non-* value in*'"
        " THEN raise(ABORT,'type mismatch on DEFAULT')"
        " ELSE raise(ABORT,'NOT NULL constraint failed')"
        " END"
        "  FROM pragma_quick_check(%Q,%Q)"
        " WHERE quick_check GLOB 'CHECK*'"
        " OR quick_check GLOB 'NULL*'"
        " OR quick_check GLOB 'non-* value in*'",
        z_tab, z_db
      );
    }
  }
}

/*
** This function is called by the parser after the table-name in
** an "ALTER TABLE <table-name> ADD" statement is parsed. Argument
** src is the full-name of the table being altered.
**
** This routine makes a (partial) copy of the table_t structure
** for the table being altered and sets parser_t.new_table to point
** to it. Routines called by the parser as the column definition
** is parsed (i.e. __add_column()) add the new column_t data to
** the copy. The copy of the table_t structure is deleted by tokenize.c
** after parsing is finished.
**
** Routine __alter_finish_add_column() will be called to complete
** coding the "ALTER TABLE ... ADD" statement.
*/
void __alter_begin_add_column(parser_t *parser, src_list_t *src){
  table_t *p_new;
  table_t *tab;
  int i_db;
  int i;
  int n_alloc;
  DBSQL *db = parser->db;

  /* Look up the table being altered. */
  assert( parser->new_table==0 );
  assert( __sm_holds_all_mutexes(db) );
  if( NEVER(db->malloc_failed) ) goto exit_begin_add_column;
  tab = __locate_table_item(parser, 0, &src->a[0]);
  if( !tab ) goto exit_begin_add_column;

#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( IS_VIRTUAL(tab) ){
    __error_msg(parser, "virtual tables may not be altered");
    goto exit_begin_add_column;
  }
#endif

  /* Make sure this is not an attempt to ALTER a view. */
  if( IsView(tab) ){
    __error_msg(parser, "Cannot add a column to a view");
    goto exit_begin_add_column;
  }
  if( DBSQL_OK!=__is_alterable_table(parser, tab) ){
    goto exit_begin_add_column;
  }

  __may_abort(parser);
  assert( IS_ORDINARY_TABLE(tab) );
  assert( tab->u.tab.add_col_offset>0 );
  i_db = __schema_to_index(db, tab->schema);

  /* Put a copy of the table_t struct in parser_t.new_table for the
  ** __add_column() function and friends to modify.  But modify
  ** the name by adding an "dbsql_altertab_" prefix.  By adding this
  ** prefix, we insure that the name will not collide with an existing
  ** table because user table are not allowed to have the "dbsql_"
  ** prefix on their name.
  */
  p_new = (table_t*)__db_malloc_zero(db, sizeof(table_t));
  if( !p_new ) goto exit_begin_add_column;
  parser->new_table = p_new;
  p_new->n_tab_ref = 1;
  p_new->n_col = tab->n_col;
  assert( p_new->n_col>0 );
  n_alloc = (((p_new->n_col-1)/8)*8)+8;
  assert( n_alloc>=p_new->n_col && n_alloc%8==0 && n_alloc-p_new->n_col<8 );
  p_new->cols = (column_t*)__db_malloc_zero(db, sizeof(column_t)*(u_int32_t)n_alloc);
  p_new->name = __m_printf(db, "dbsql_altertab_%s", tab->name);
  if( !p_new->cols || !p_new->name ){
    assert( db->malloc_failed );
    goto exit_begin_add_column;
  }
  memcpy(p_new->cols, tab->cols, sizeof(column_t)*(size_t)p_new->n_col);
  for(i=0; i<p_new->n_col; i++){
    column_t *col = &p_new->cols[i];
    col->cn_name = __db_str_dup(db, col->cn_name);
    col->h_name = __str_i_hash(col->cn_name);
  }
  assert( IS_ORDINARY_TABLE(p_new) );
  p_new->u.tab.dflt_list = __expr_list_dup(db, tab->u.tab.dflt_list, 0);
  p_new->schema = db->dbs[i_db].schema;
  p_new->u.tab.add_col_offset = tab->u.tab.add_col_offset;
  assert( p_new->n_tab_ref==1 );

exit_begin_add_column:
  __src_list_delete(db, src);
  return;
}

/*
** Parameter tab is the subject of an ALTER TABLE ... RENAME COLUMN
** command. This function checks if the table is a view or virtual
** table (columns of views or virtual tables may not be renamed). If so,
** it loads an error message into parser and returns non-zero.
**
** Or, if tab is not a view or virtual table, zero is returned.
*/
#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_VIRTUALTABLE)
static int IS_REAL_TABLE(parser_t *parser, table_t *tab, int i_op){
  const char *type = 0;
#ifndef DBSQL_OMIT_VIEW
  if( IsView(tab) ){
    type = "view";
  }
#endif
#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( IS_VIRTUAL(tab) ){
    type = "virtual table";
  }
#endif
  if( type ){
    const char *msgs[] = {
      "rename columns of", "drop column from", "edit constraints of"
    };
    assert( i_op>=0 && i_op<ARRAY_SIZE(msgs) );
    __error_msg(parser, "cannot %s %s \"%s\"",
        msgs[i_op], type, tab->name
    );
    return 1;
  }
  return 0;
}
#else /* !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_VIRTUALTABLE) */
# define IS_REAL_TABLE(x,y,z) (0)
#endif

/*
** Handles the following parser reduction:
**
**  cmd ::= ALTER TABLE src RENAME COLUMN old TO p_new
*/
void __alter_rename_column(
  parser_t *parser,                  /* Parsing context */
  src_list_t *src,                  /* table_t being altered.  src->n_src==1 */
  Token *old,                    /* Name of column being changed */
  Token *p_new                     /* New column name */
){
  DBSQL *db = parser->db;       /* Database connection */
  table_t *tab;                    /* table_t being updated */
  int i_col;                       /* index_t of column being renamed */
  char *z_old = 0;                 /* Old column name */
  char *z_new = 0;                 /* New column name */
  const char *z_db;                /* Name of schema containing the table */
  int i_schema;                    /* index_t of the schema */
  int quote;                     /* True to quote the new name */

  /* Locate the table to be altered */
  tab = __locate_table_item(parser, 0, &src->a[0]);
  if( !tab ) goto exit_rename_column;

  /* Cannot alter a system table */
  if( DBSQL_OK!=__is_alterable_table(parser, tab) ) goto exit_rename_column;
  if( DBSQL_OK!=IS_REAL_TABLE(parser, tab, 0) ) goto exit_rename_column;

  /* Which schema holds the table to be altered */ 
  i_schema = __schema_to_index(db, tab->schema);
  assert( i_schema>=0 );
  z_db = db->dbs[i_schema].db_s_name;

#ifndef DBSQL_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( __auth_check(parser, DBSQL_ALTER_TABLE, z_db, tab->name, 0) ){
    goto exit_rename_column;
  }
#endif

  /* Make sure the old name really is a column name in the table to be
  ** altered.  Set i_col to be the index of the column being renamed */
  z_old = __name_from_token(db, old);
  if( !z_old ) goto exit_rename_column;
  i_col = __column_index(tab, z_old);
  if( i_col<0 ){
    __error_msg(parser, "no such column: \"%T\"", old);
    goto exit_rename_column;
  }

  /* Ensure the schema contains no double-quoted strings */
  __rename_test_schema(parser, z_db, i_schema==1, "", 0);
  __rename_fix_quotes(parser, z_db, i_schema==1);

  /* Do the rename operation using a recursive UPDATE statement that
  ** uses the dbsql_rename_column() SQL function to compute the new
  ** CREATE statement text for the dbsql_schema table.
  */
  __may_abort(parser);
  z_new = __name_from_token(db, p_new);
  if( !z_new ) goto exit_rename_column;
  assert( p_new->n>0 );
  quote = __isquote(p_new->z[0]);
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, %d) "
      "WHERE name NOT LIKE 'dbsqlX_%%' ESCAPE 'X' "
      " AND (type != 'index' OR tbl_name = %Q)",
      z_db,
      z_db, tab->name, i_col, z_new, quote, i_schema==1,
      tab->name
  );

  __nested_parse(parser,
      "UPDATE temp." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, 1) "
      "WHERE type IN ('trigger', 'view')",
      z_db, tab->name, i_col, z_new, quote
  );

  /* Drop and reload the database schema. */
  __rename_reload_schema(parser, i_schema, INITFLAG_AlterRename);
  __rename_test_schema(parser, z_db, i_schema==1, "after rename", 1);

 exit_rename_column:
  __src_list_delete(db, src);
  __dbsql_db_free(db, z_old);
  __dbsql_db_free(db, z_new);
  return;
}

/*
** Each rename_token_t object maps an element of the parse tree into
** the token that generated that element.  The parse tree element
** might be one of:
**
**     *  A pointer to an expr_t that represents an ID
**     *  The name of a table column in column_t.name
**
** A list of rename_token_t objects can be constructed during parsing.
** Each new object is created by __rename_token_map().
** As the parse tree is transformed, the __rename_token_remap()
** routine is used to keep the mapping current.
**
** After the parse finishes, __rename_token_find() routine can be used
** to look up the actual token value that created some element in
** the parse tree.
*/
struct rename_token_t {
  const void *p;         /* parser_t tree element created by token t */
  Token t;               /* The token that created parse tree element p */
  rename_token_t *next;    /* Next is a list of all rename_token_t objects */
};

/*
** The context of an ALTER TABLE RENAME COLUMN operation that gets passed
** down into the walker_t.
*/
typedef struct rename_ctx_t rename_ctx_t;
struct rename_ctx_t {
  rename_token_t *list;             /* List of tokens to overwrite */
  int n_list;                      /* Number of tokens in list */
  int i_col;                       /* index_t of column being renamed */
  table_t *tab;                    /* table_t being ALTERed */
  const char *old;               /* Old column name */
};

#ifdef DBSQL_DEBUG
/*
** This function is only for debugging. It performs two tasks:
**
**   1. Checks that pointer ptr does not already appear in the
**      rename-token list.
**
**   2. Dereferences each pointer in the rename-token list.
**
** The second is most effective when debugging under valgrind or
** address-sanitizer or similar. If any of these pointers no longer
** point to valid objects, an exception is raised by the memory-checking
** tool.
**
** The point of this is to prevent comparisons of invalid pointer values.
** Even though this always seems to work, it is undefined according to the
** C standard. Example of undefined comparison:
**
**     dbsql_free(x);
**     if( x==y ) ...
**
** Technically, as x no longer points into a valid object or to the byte
** following a valid object, it may not be used in comparison operations.
*/
static void RENAME_TOKEN_CHECK_ALL(parser_t *parser, const void *ptr){
  assert( parser==parser->db->parser );
  assert( parser->db->malloc_failed==0 || parser->n_err!=0 );
  if( parser->n_err==0 ){
    const rename_token_t *p;
    u_int32_t i = 1;
    for(p=parser->rename; p; p=p->next){
      if( p->p ){
        assert( p->p!=ptr );
        i += *(u_int8_t*)(p->p) | 1;
      }
    }
    assert( i>0 );
  }
}
#else
# define RENAME_TOKEN_CHECK_ALL(x,y)
#endif

/*
** Remember that the parser tree element ptr was created using
** the token token.
**
** In other words, construct a new rename_token_t object and add it
** to the list of rename_token_t objects currently being built up
** in parser->rename.
**
** The ptr argument is returned so that this routine can be used
** with tail recursion in __token_expr() routine, for a small performance
** improvement.
*/
const void *__rename_token_map(
  parser_t *parser,
  const void *ptr,
  const Token *token
){
  rename_token_t *p_new;
  assert( ptr || parser->db->malloc_failed );
  RENAME_TOKEN_CHECK_ALL(parser, ptr);
  if( ALWAYS(parser->parse_mode!=PARSE_MODE_UNMAP) ){
    p_new = __db_malloc_zero(parser->db, sizeof(rename_token_t));
    if( p_new ){
      p_new->p = ptr;
      p_new->t = *token;
      p_new->next = parser->rename;
      parser->rename = p_new;
    }
  }

  return ptr;
}

/*
** It is assumed that there is already a rename_token_t object associated
** with parse tree element from. This function remaps the associated token
** to parse tree element to.
*/
void __rename_token_remap(parser_t *parser, const void *to, const void *from){
  rename_token_t *p;
  RENAME_TOKEN_CHECK_ALL(parser, to);
  for(p=parser->rename; p; p=p->next){
    if( p->p==from ){
      p->p = to;
      break;
    }
  }
}

/*
** walker_t callback used by __rename_expr_unmap().
*/
static int __rename_unmap_expr_cb(walker_t *walker, expr_t *expr){
  parser_t *parser = walker->parser;
  __rename_token_remap(parser, 0, (const void*)expr);
  if( EXPR_USE_Y_TAB(expr) ){
    __rename_token_remap(parser, 0, (const void*)&expr->y.tab);
  }
  return WRC_Continue;
}

/*
** Iterate through the select_t objects that are part of WITH clauses attached
** to select statement p_select.
*/
static void __rename_walk_with(walker_t *walker, select_t *p_select){
  with_t *with = p_select->with;
  if( with ){
    parser_t *parser = walker->parser;
    int i;
    with_t *copy = 0;
    assert( with->n_cte>0 );
    if( (with->a[0].p_select->sel_flags & SF_Expanded)==0 ){
      /* Push a copy of the with_t object onto the with-stack. We use a copy
      ** here as the original will be expanded and resolved (flags SF_Expanded
      ** and SF_Resolved) below. And the parser code that uses the with-stack
      ** fails if the select_t objects on it have already been expanded and
      ** resolved.  */
      copy = __with_dup(parser->db, with);
      copy = __with_push(parser, copy, 1);
    }
    for(i=0; i<with->n_cte; i++){
      select_t *p = with->a[i].p_select;
      name_context_t s_nc;
      memset(&s_nc, 0, sizeof(s_nc));
      s_nc.parser = parser;
      if( copy ) __select_prep(s_nc.parser, p, &s_nc);
      if( s_nc.parser->db->malloc_failed ) return;
      __walk_select(walker, p);
      __rename_exprlist_unmap(parser, with->a[i].cols);
    }
    if( copy && parser->with==copy ){
      parser->with = copy->outer;
    }
  }
}

/*
** Unmap all tokens in the id_list_t object passed as the second argument.
*/
static void __unmap_column_idlist_names(
  parser_t *parser,
  const id_list_t *id_list
){
  int ii;
  assert( id_list!=0 );
  for(ii=0; ii<id_list->n_id; ii++){
    __rename_token_remap(parser, 0, (const void*)id_list->a[ii].name);
  }
}

/*
** walker_t callback used by __rename_expr_unmap().
*/
static int __rename_unmap_select_cb(walker_t *walker, select_t *p){
  parser_t *parser = walker->parser;
  int i;
  if( parser->n_err ) return WRC_Abort;
  testcase( p->sel_flags & SF_View );
  testcase( p->sel_flags & SF_CopyCte );
  if( p->sel_flags & (SF_View|SF_CopyCte) ){
    return WRC_Prune;
  }
  if( ALWAYS(p->e_list) ){
    expr_list_t *list = p->e_list;
    for(i=0; i<list->n_expr; i++){
      if( list->a[i].e_name && list->a[i].fg.e_name==ENAME_NAME ){
        __rename_token_remap(parser, 0, (void*)list->a[i].e_name);
      }
    }
  }
  if( ALWAYS(p->src) ){  /* Every select_t as a src_list_t, even if it is empty */
    src_list_t *src = p->src;
    for(i=0; i<src->n_src; i++){
      __rename_token_remap(parser, 0, (void*)src->a[i].name);
      if( src->a[i].fg.is_using==0 ){
        __walk_expr(walker, src->a[i].u3.on);
      }else{
        __unmap_column_idlist_names(parser, src->a[i].u3.p_using);
      }
    }
  }

  __rename_walk_with(walker, p);
  return WRC_Continue;
}

/*
** Remove all nodes that are part of expression expr from the rename list.
*/
void __rename_expr_unmap(parser_t *parser, expr_t *expr){
  u_int8_t mode = parser->parse_mode;
  walker_t s_walker;
  memset(&s_walker, 0, sizeof(walker_t));
  s_walker.parser = parser;
  s_walker.expr_callback = __rename_unmap_expr_cb;
  s_walker.select_callback = __rename_unmap_select_cb;
  parser->parse_mode = PARSE_MODE_UNMAP;
  __walk_expr(&s_walker, expr);
  parser->parse_mode = mode;
}

/*
** Remove all nodes that are part of expression-list e_list from the
** rename list.
*/
void __rename_exprlist_unmap(parser_t *parser, expr_list_t *e_list){
  if( e_list ){
    int i;
    walker_t s_walker;
    memset(&s_walker, 0, sizeof(walker_t));
    s_walker.parser = parser;
    s_walker.expr_callback = __rename_unmap_expr_cb;
    __walk_expr_list(&s_walker, e_list);
    for(i=0; i<e_list->n_expr; i++){
      if( ALWAYS(e_list->a[i].fg.e_name==ENAME_NAME) ){
        __rename_token_remap(parser, 0, (void*)e_list->a[i].e_name);
      }
    }
  }
}

/*
** Free the list of rename_token_t objects given in the second argument
*/
static void __rename_token_free(DBSQL *db, rename_token_t *token){
  rename_token_t *next;
  rename_token_t *p;
  for(p=token; p; p=next){
    next = p->next;
    __dbsql_db_free(db, p);
  }
}

/*
** Search the parser_t object passed as the first argument for a rename_token_t
** object associated with parse tree element ptr. If found, return a pointer
** to it. Otherwise, return NULL.
**
** If the second argument passed to this function is not NULL and a matching
** rename_token_t object is found, remove it from the parser_t object and add it to
** the list maintained by the rename_ctx_t object.
*/
static rename_token_t *__rename_token_find(
  parser_t *parser,
  struct rename_ctx_t *ctx,
  const void *ptr
){
  rename_token_t **pp;
  if( NEVER(ptr==0) ){
    return 0;
  }
  for(pp=&parser->rename; (*pp); pp=&(*pp)->next){
    if( (*pp)->p==ptr ){
      rename_token_t *token = *pp;
      if( ctx ){
        *pp = token->next;
        token->next = ctx->list;
        ctx->list = token;
        ctx->n_list++;
      }
      return token;
    }
  }
  return 0;
}

/*
** This is a walker_t select callback. It does nothing. It is only required
** because without a dummy callback, __walk_expr() and similar do not
** descend into sub-select statements.
*/
static int __rename_column_select_cb(walker_t *walker, select_t *p){
  if( p->sel_flags & (SF_View|SF_CopyCte) ){
    testcase( p->sel_flags & SF_View );
    testcase( p->sel_flags & SF_CopyCte );
    return WRC_Prune;
  }
  __rename_walk_with(walker, p);
  return WRC_Continue;
}

/*
** This is a walker_t expression callback.
**
** For every TK_COLUMN node in the expression tree, search to see
** if the column being references is the column being renamed by an
** ALTER TABLE statement.  If it is, then attach its associated
** rename_token_t object to the list of rename_token_t objects being
** constructed in rename_ctx_t object at walker->u.rename.
*/
static int __rename_column_expr_cb(walker_t *walker, expr_t *expr){
  rename_ctx_t *p = walker->u.rename;
  if( expr->op==TK_TRIGGER
   && expr->i_column==p->i_col
   && walker->parser->trigger_tab==p->tab
  ){
    __rename_token_find(walker->parser, p, (void*)expr);
  }else if( expr->op==TK_COLUMN
   && expr->i_column==p->i_col
   && ALWAYS(EXPR_USE_Y_TAB(expr))
   && p->tab==expr->y.tab
  ){
    __rename_token_find(walker->parser, p, (void*)expr);
  }
  return WRC_Continue;
}

/*
** The rename_ctx_t contains a list of tokens that reference a column that
** is being renamed by an ALTER TABLE statement.  Return the "last"
** rename_token_t in the rename_ctx_t and remove that rename_token_t from the
** __rename_context.  "Last" means the last rename_token_t encountered when
** the input SQL is parsed from left to right.  Repeated calls to this routine
** return all column name tokens in the order that they are encountered
** in the SQL statement.
*/
static rename_token_t *__rename_column_token_next(rename_ctx_t *ctx){
  rename_token_t *best = ctx->list;
  rename_token_t *token;
  rename_token_t **pp;

  for(token=best->next; token; token=token->next){
    if( token->t.z>best->t.z ) best = token;
  }
  for(pp=&ctx->list; *pp!=best; pp=&(*pp)->next);
  *pp = best->next;

  return best;
}

/*
** Set the error message of the context passed as the first argument to
** the result of formatting fmt using printf() style formatting.
*/
static void __error_m_printf(dbsql_func_t *ctx, const char *fmt, ...){
  DBSQL *db = dbsql_context_db_handle(ctx);
  char *err = 0;
  va_list ap;
  va_start(ap, fmt);
  err = __vm_printf(db, fmt, ap);
  va_end(ap);
  if( err ){
    dbsql_result_error(ctx, err, -1);
    __dbsql_db_free(db, err);
  }else{
    dbsql_result_error_nomem(ctx);
  }
}

/*
** An error occurred while parsing or otherwise processing a database
** object (either parser->new_table, new_index or new_trigger) as part of an
** ALTER TABLE RENAME COLUMN program. The error message emitted by the
** sub-routine is currently stored in parser->errmsg. This function
** adds context to the error message and then stores it in ctx.
*/
static void __rename_column_parse_error(
  dbsql_func_t *ctx,
  const char *when,
  dbsql_value_t *type,
  dbsql_value_t *object,
  parser_t *parser
){
  const char *t = (const char*)dbsql_value_text(type);
  const char *n = (const char*)dbsql_value_text(object);
  char *err;

  err = __m_printf(parser->db, "error in %s %s%s%s: %s",
      t, n, (when[0] ? " " : ""), when,
      parser->errmsg
  );
  dbsql_result_error(ctx, err, -1);
  __dbsql_db_free(parser->db, err);
}

/*
** For each name in the the expression-list e_list (i.e. each
** e_list->a[i].name) that matches the string in old, extract the
** corresponding rename-token from parser_t object parser and add it
** to the rename_ctx_t ctx.
*/
static void __rename_column_elist_names(
  parser_t *parser,
  rename_ctx_t *ctx,
  const expr_list_t *e_list,
  const char *old
){
  if( e_list ){
    int i;
    for(i=0; i<e_list->n_expr; i++){
      const char *name = e_list->a[i].e_name;
      if( ALWAYS(e_list->a[i].fg.e_name==ENAME_NAME)
       && ALWAYS(name!=0)
       && 0==dbsql_stricmp(name, old)
      ){
        __rename_token_find(parser, ctx, (const void*)name);
      }
    }
  }
}

/*
** For each name in the the id-list id_list (i.e. each id_list->a[i].name)
** that matches the string in old, extract the corresponding rename-token
** from parser_t object parser and add it to the rename_ctx_t ctx.
*/
static void __rename_column_idlist_names(
  parser_t *parser,
  rename_ctx_t *ctx,
  const id_list_t *id_list,
  const char *old
){
  if( id_list ){
    int i;
    for(i=0; i<id_list->n_id; i++){
      const char *name = id_list->a[i].name;
      if( 0==dbsql_stricmp(name, old) ){
        __rename_token_find(parser, ctx, (const void*)name);
      }
    }
  }
}


/*
** parser_t the SQL statement sql using parser_t object (*p). The parser_t object
** is initialized by this function before it is used.
*/
static int __rename_parse_sql(
  parser_t *p,                       /* Memory to use for parser_t object */
  const char *z_db,                /* Name of schema SQL belongs to */
  DBSQL *db,                    /* Database handle */
  const char *sql,               /* SQL to parse */
  int temp                       /* True if SQL is from temp schema */
){
  int rc;
  u_int64_t flags;

  __parse_object_init(p, db);
  if( sql==0 ){
    return DBSQL_NOMEM;
  }
  if( __str_nicmp(sql,"CREATE ",7)!=0 ){
    return DBSQL_CORRUPT_BKPT;
  }
  if( temp ){
    db->init.i_db = 1;
  }else{
    int i_db = __find_db_name(db, z_db);
    assert( i_db>=0 && i_db<=0xff );
    db->init.i_db = (u_int8_t)i_db;
  }
  p->parse_mode = PARSE_MODE_RENAME;
  p->db = db;
  p->n_query_loop = 1;
  flags = db->flags;
  testcase( (db->flags & DBSQL_Comments)==0 && strstr(sql," /* ")!=0 );
  db->flags |= DBSQL_Comments;
  rc = __run_parser(p, sql);
  db->flags = flags;
  if( db->malloc_failed ) rc = DBSQL_NOMEM;
  if( rc==DBSQL_OK
   && NEVER(p->new_table==0 && p->new_index==0 && p->new_trigger==0)
  ){
    rc = DBSQL_CORRUPT_BKPT;
  }

#ifdef DBSQL_DEBUG
  /* Ensure that all mappings in the parser_t.rename list really do map to
  ** a part of the input string.  */
  if( rc==DBSQL_OK ){
    int n_sql = __strlen30(sql);
    rename_token_t *token;
    for(token=p->rename; token; token=token->next){
      assert( token->t.z>=sql && &token->t.z[token->t.n]<=&sql[n_sql] );
    }
  }
#endif

  db->init.i_db = 0;
  return rc;
}

/*
** This function edits SQL statement sql, replacing each token identified
** by the linked list rename with the text of z_new. If argument quote is
** true, then z_new is always quoted first. If no error occurs, the result
** is loaded into context object ctx as the result.
**
** Or, if an error occurs (i.e. an OOM condition), an error is left in
** ctx and an dbsql error code returned.
*/
static int __rename_edit_sql(
  dbsql_func_t *ctx,          /* Return result here */
  rename_ctx_t *rename,             /* Rename context */
  const char *sql,               /* SQL statement to edit */
  const char *z_new,               /* New token text */
  int quote                      /* True to always quote token */
){
  int64_t n_new = __strlen30(z_new);
  int64_t n_sql = __strlen30(sql);
  DBSQL *db = dbsql_context_db_handle(ctx);
  int rc = DBSQL_OK;
  char *quot = 0;
  char *out;
  int64_t n_quot = 0;
  char *buf1 = 0;
  char *buf2 = 0;

  if( z_new ){
    /* Set quot to point to a buffer containing a quoted copy of the
    ** identifier z_new. If the corresponding identifier in the original
    ** ALTER TABLE statement was quoted (quote==1), then set z_new to
    ** point to quot so that all substitutions are made using the
    ** quoted version of the new column name.  */
    quot = __m_printf(db, "\"%w\" ", z_new);
    if( quot==0 ){
      return DBSQL_NOMEM;
    }else{
      n_quot = __strlen30(quot)-1;
    }

    assert( n_quot>=n_new && n_sql>=0 && n_new>=0 );
    out = __db_malloc_zero(db, (u_int64_t)n_sql + rename->n_list*(u_int64_t)n_quot + 1);
  }else{
    assert( n_sql>0 );
    out = (char*)__db_malloc_zero(db, (2*(u_int64_t)n_sql + 1) * 3);
    if( out ){
      buf1 = &out[n_sql*2+1];
      buf2 = &out[n_sql*4+2];
    }
  }

  /* At this point rename->list contains a list of rename_token_t objects
  ** corresponding to all tokens in the input SQL that must be replaced
  ** with the new column name, or with single-quoted versions of themselves.
  ** All that remains is to construct and return the edited SQL string. */
  if( out ){
    int64_t n_out = n_sql;
    assert( n_sql>0 );
    memcpy(out, sql, (size_t)n_sql);
    while( rename->list ){
      int i_off;                   /* Offset of token to replace in out */
      int64_t n_replace;
      const char *replace;
      rename_token_t *best = __rename_column_token_next(rename);

      if( z_new ){
        if( quote==0 && __is_id_char(*(u_int8_t*)best->t.z) ){
          n_replace = n_new;
          replace = z_new;
        }else{
          n_replace = n_quot;
          replace = quot;
          if( best->t.z[best->t.n]=='"' ) n_replace++;
        }
      }else{
        /* Dequote the double-quoted token. Then requote it again, this time
        ** using single quotes. If the character immediately following the
        ** original token within the input SQL was a single quote ('), then
        ** add another space after the new, single-quoted version of the
        ** token. This is so that (SELECT "string"'alias') maps to
        ** (SELECT 'string' 'alias'), and not (SELECT 'string''alias').  */
        memcpy(buf1, best->t.z, best->t.n);
        buf1[best->t.n] = 0;
        __dequote(buf1);
        assert( n_sql < 0x15555554 /* otherwise malloc would have failed */ );
        dbsql_snprintf((int)(n_sql*2), buf2, "%Q%s", buf1,
            best->t.z[best->t.n]=='\'' ? " " : ""
        );
        replace = buf2;
        n_replace = __strlen30(replace);
      }

      i_off = (int)(best->t.z - sql);
      if( best->t.n!=n_replace ){
        memmove(&out[i_off + n_replace], &out[i_off + best->t.n],
            n_out - (i_off + best->t.n)
        );
        n_out += n_replace - best->t.n;
        out[n_out] = '\0';
      }
      memcpy(&out[i_off], replace, n_replace);
      __dbsql_db_free(db, best);
    }

    dbsql_result_text(ctx, out, -1, DBSQL_TRANSIENT);
    __dbsql_db_free(db, out);
  }else{
    rc = DBSQL_NOMEM;
  }

  dbsql_free(quot);
  return rc;
}

/*
** Set all e_list->a[].fg.e_name fields in the expression-list to val.
*/
static void __rename_set_e_names(expr_list_t *e_list, int val){
  assert( val==ENAME_NAME || val==ENAME_TAB || val==ENAME_SPAN );
  if( e_list ){
    int i;
    for(i=0; i<e_list->n_expr; i++){
      assert( val==ENAME_NAME || e_list->a[i].fg.e_name==ENAME_NAME );
      e_list->a[i].fg.e_name = val&0x3;
    }
  }
}

/*
** Resolve all symbols in the trigger at parser->new_trigger, assuming
** it was read from the schema of database db. Return DBSQL_OK if
** successful. Otherwise, return an dbsql error code and leave an error
** message in the parser_t object.
*/
static int __rename_resolve_trigger(parser_t *parser){
  DBSQL *db = parser->db;
  trigger_t *p_new = parser->new_trigger;
  trigger_step_t *step;
  name_context_t s_nc;
  int rc = DBSQL_OK;

  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.parser = parser;
  assert( p_new->tab_schema );
  parser->trigger_tab = __find_table(db, p_new->table,
      db->dbs[__schema_to_index(db, p_new->tab_schema)].db_s_name
  );
  parser->trigger_op = p_new->op;
  /* ALWAYS() because if the table of the trigger does not exist, the
  ** error would have been hit before this point */
  if( ALWAYS(parser->trigger_tab) ){
    rc = __view_get_column_names(parser, parser->trigger_tab)!=0;
  }

  /* Resolve symbols in WHEN clause */
  if( rc==DBSQL_OK && p_new->when ){
    rc = __resolve_expr_names(&s_nc, p_new->when);
  }

  for(step=p_new->step_list; rc==DBSQL_OK && step; step=step->next){
    if( step->p_select ){
      __select_prep(parser, step->p_select, &s_nc);
      if( parser->n_err ) rc = parser->rc;
    }
    if( rc==DBSQL_OK && step->src ){
      src_list_t *src = __src_list_dup(db, step->src, 0);
      if( src ){
        select_t *sel = __select_new(
            parser, step->expr_list, src, 0, 0, 0, 0, 0, 0
        );
        if( sel==0 ){
          step->expr_list = 0;
          src = 0;
          rc = DBSQL_NOMEM;
        }else{
          /* step->expr_list contains an expression-list used for an UPDATE
          ** statement. So the a[].e_name values are the RHS of the
          ** "<col> = <expr>" clauses of the UPDATE statement. So, before
          ** running __select_prep(), change all the e_name values in
          ** step->expr_list to ENAME_SPAN (from their current value of
          ** ENAME_NAME). This is to prevent any ids in ON() clauses that are
          ** part of src from being incorrectly resolved against the
          ** a[].e_name values as if they were column aliases.  */
          __rename_set_e_names(step->expr_list, ENAME_SPAN);
          __select_prep(parser, sel, 0);
          __rename_set_e_names(step->expr_list, ENAME_NAME);
          rc = parser->n_err ? DBSQL_ERROR : DBSQL_OK;
          assert( step->expr_list==0 || step->expr_list==sel->e_list );
          assert( src==sel->src );
          if( step->expr_list ) sel->e_list = 0;
          sel->src = 0;
          __select_delete(db, sel);
        }
        if( ALWAYS(step->src) ){
          int i;
          for(i=0; i<step->src->n_src && rc==DBSQL_OK; i++){
            src_item_t *p = &step->src->a[i];
            if( p->fg.is_subquery ){
              assert( p->u4.subq!=0 );
              __select_prep(parser, p->u4.subq->p_select, 0);
            }
          }
        }

        if(  db->malloc_failed ){
          rc = DBSQL_NOMEM;
        }
        s_nc.src_list = src;
        if( rc==DBSQL_OK && step->where ){
          rc = __resolve_expr_names(&s_nc, step->where);
        }
        if( rc==DBSQL_OK ){
          rc = __resolve_expr_list_names(&s_nc, step->expr_list);
        }
        assert( !step->upsert || (!step->where && !step->expr_list) );
        if( step->upsert && rc==DBSQL_OK ){
          upsert_t *upsert = step->upsert;
          upsert->upsert_src = src;
          s_nc.u_nc.upsert = upsert;
          s_nc.nc_flags = NC_UUpsert;
          rc = __resolve_expr_list_names(&s_nc, upsert->upsert_target);
          if( rc==DBSQL_OK ){
            expr_list_t *upsert_set = upsert->upsert_set;
            rc = __resolve_expr_list_names(&s_nc, upsert_set);
          }
          if( rc==DBSQL_OK ){
            rc = __resolve_expr_names(&s_nc, upsert->upsert_where);
          }
          if( rc==DBSQL_OK ){
            rc = __resolve_expr_names(&s_nc, upsert->upsert_target_where);
          }
          s_nc.nc_flags = 0;
        }
        s_nc.src_list = 0;
        __src_list_delete(db, src);
      }else{
        rc = DBSQL_NOMEM;
      }
    }
  }
  return rc;
}

/*
** Invoke __walk_expr() or __walk_select() on all select_t or expr_t
** objects that are part of the trigger passed as the second argument.
*/
static void __rename_walk_trigger(walker_t *walker, trigger_t *P_TRIGGER){
  trigger_step_t *step;

  /* Find tokens to edit in WHEN clause */
  __walk_expr(walker, P_TRIGGER->when);

  /* Find tokens to edit in trigger steps */
  for(step=P_TRIGGER->step_list; step; step=step->next){
    __walk_select(walker, step->p_select);
    __walk_expr(walker, step->where);
    __walk_expr_list(walker, step->expr_list);
    if( step->upsert ){
      upsert_t *upsert = step->upsert;
      __walk_expr_list(walker, upsert->upsert_target);
      __walk_expr_list(walker, upsert->upsert_set);
      __walk_expr(walker, upsert->upsert_where);
      __walk_expr(walker, upsert->upsert_target_where);
    }
    if( step->src ){
      int i;
      src_list_t *src = step->src;
      for(i=0; i<src->n_src; i++){
        if( src->a[i].fg.is_subquery ){
          assert( src->a[i].u4.subq!=0 );
          __walk_select(walker, src->a[i].u4.subq->p_select);
        }
      }
    }
  }
}

/*
** Free the contents of parser_t object (*parser). Do not free the memory
** occupied by the parser_t object itself.
*/
static void __rename_parse_cleanup(parser_t *parser){
  DBSQL *db = parser->db;
  index_t *idx;
  if( parser->vdbe ){
    __vdbe_finalize(parser->vdbe);
  }
  __delete_table(db, parser->new_table);
  while( (idx = parser->new_index)!=0 ){
    parser->new_index = idx->next;
    __free_index(db, idx);
  }
  __delete_trigger(db, parser->new_trigger);
  __dbsql_db_free(db, parser->errmsg);
  __rename_token_free(db, parser->rename);
  __parse_object_reset(parser);
}

/*
** SQL function:
**
**     dbsql_rename_column(SQL,TYPE,OBJ,DB,TABLE,COL,NEWNAME,QUOTE,TEMP)
**
**   0. sql:     SQL statement to rewrite
**   1. type:     Type of object ("table", "view" etc.)
**   2. object:   Name of object
**   3. Database: Database name (e.g. "main")
**   4. table_t:    table_t name
**   5. i_col:     index_t of column to rename
**   6. z_new:     New column name
**   7. quote:   Non-zero if the new column name should be quoted.
**   8. temp:    True if sql comes from temp schema
**
** Do a column rename operation on the CREATE statement given in sql.
** The i_col-th column (left-most is 0) of table table is renamed from col
** into z_new.  The name should be quoted if quote is true.
**
** This function is used internally by the ALTER TABLE RENAME COLUMN command.
** It is only accessible to SQL created using __nested_parse().  It is
** not reachable from ordinary SQL passed into dbsql_prepare() unless the
** DBSQL_TESTCTRL_INTERNAL_FUNCTIONS test setting is enabled.
*/
static void __rename_column_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  rename_ctx_t s_ctx;
  const char *sql = (const char*)dbsql_value_text(argv[0]);
  const char *z_db = (const char*)dbsql_value_text(argv[3]);
  const char *table = (const char*)dbsql_value_text(argv[4]);
  int i_col = dbsql_value_int(argv[5]);
  const char *z_new = (const char*)dbsql_value_text(argv[6]);
  int quote = dbsql_value_int(argv[7]);
  int temp = dbsql_value_int(argv[8]);
  const char *old;
  int rc;
  parser_t s_parse;
  walker_t s_walker;
  index_t *idx;
  int i;
  table_t *tab;
#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth = db->auth;
#endif

  UNUSED_PARAMETER(__not_used);
  if( sql==0 ) return;
  if( table==0 ) return;
  if( z_new==0 ) return;
  if( i_col<0 ) return;
  __sm_enter_all(db);
  tab = __find_table(db, table, z_db);
  if( tab==0 || i_col>=tab->n_col ){
    __sm_leave_all(db);
    return;
  }
  old = tab->cols[i_col].cn_name;
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.i_col = ((i_col==tab->i_p_key) ? -1 : i_col);

#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = 0;
#endif
  rc = __rename_parse_sql(&s_parse, z_db, db, sql, temp);

  /* Find tokens that need to be replaced. */
  memset(&s_walker, 0, sizeof(walker_t));
  s_walker.parser = &s_parse;
  s_walker.expr_callback = __rename_column_expr_cb;
  s_walker.select_callback = __rename_column_select_cb;
  s_walker.u.rename = &s_ctx;

  s_ctx.tab = tab;
  if( rc!=DBSQL_OK ) goto rename_column_func_done;
  if( s_parse.new_table ){
    if( IsView(s_parse.new_table) ){
      select_t *p_select = s_parse.new_table->u.view.p_select;
      p_select->sel_flags &= ~(u_int32_t)SF_View;
      s_parse.rc = DBSQL_OK;
      __select_prep(&s_parse, p_select, 0);
      rc = (db->malloc_failed ? DBSQL_NOMEM : s_parse.rc);
      if( rc==DBSQL_OK ){
        __walk_select(&s_walker, p_select);
      }
      if( rc!=DBSQL_OK ) goto rename_column_func_done;
    }else if( IS_ORDINARY_TABLE(s_parse.new_table) ){
      /* A regular table */
      int fk_only = dbsql_stricmp(table, s_parse.new_table->name);
      f_key_t *f_key;
      s_ctx.tab = s_parse.new_table;
      if( fk_only==0 ){
        if( i_col<s_parse.new_table->n_col ){
          __rename_token_find(
              &s_parse, &s_ctx, (void*)s_parse.new_table->cols[i_col].cn_name
          );
        }
        if( s_ctx.i_col<0 ){
          __rename_token_find(&s_parse, &s_ctx, (void*)&s_parse.new_table->i_p_key);
        }
        __walk_expr_list(&s_walker, s_parse.new_table->check);
        for(idx=s_parse.new_table->p_index; idx; idx=idx->next){
          __walk_expr_list(&s_walker, idx->col_exprs);
        }
        for(idx=s_parse.new_index; idx; idx=idx->next){
          __walk_expr_list(&s_walker, idx->col_exprs);
        }
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
        for(i=0; i<s_parse.new_table->n_col; i++){
          expr_t *expr = __column_expr(s_parse.new_table,
                                                  &s_parse.new_table->cols[i]);
          __walk_expr(&s_walker, expr);
        }
#endif
      }

      assert( IS_ORDINARY_TABLE(s_parse.new_table) );
      for(f_key=s_parse.new_table->u.tab.f_key; f_key; f_key=f_key->next_from){
        for(i=0; i<f_key->n_col; i++){
          if( fk_only==0 && f_key->cols[i].i_from==i_col ){
            __rename_token_find(&s_parse, &s_ctx, (void*)&f_key->cols[i]);
          }
          if( 0==dbsql_stricmp(f_key->to, table)
           && 0==dbsql_stricmp(f_key->cols[i].col, old)
          ){
            __rename_token_find(&s_parse, &s_ctx, (void*)f_key->cols[i].col);
          }
        }
      }
    }
  }else if( s_parse.new_index ){
    __walk_expr_list(&s_walker, s_parse.new_index->col_exprs);
    __walk_expr(&s_walker, s_parse.new_index->part_idx_where);
  }else{
    /* A trigger */
    trigger_step_t *step;
    rc = __rename_resolve_trigger(&s_parse);
    if( rc!=DBSQL_OK ) goto rename_column_func_done;

    for(step=s_parse.new_trigger->step_list; step; step=step->next){
      if( step->src ){
        table_t *target = __locate_table_item(&s_parse, 0, &step->src->a[0]);
        if( target==tab ){
          if( step->upsert ){
            expr_list_t *upsert_set = step->upsert->upsert_set;
            __rename_column_elist_names(&s_parse, &s_ctx, upsert_set, old);
          }
          __rename_column_idlist_names(&s_parse, &s_ctx, step->id_list, old);
          __rename_column_elist_names(&s_parse, &s_ctx, step->expr_list, old);
        }
      }
    }

    /* Find tokens to edit in UPDATE OF clause */
    if( s_parse.trigger_tab==tab ){
      __rename_column_idlist_names(&s_parse, &s_ctx,s_parse.new_trigger->columns,old);
    }

    /* Find tokens to edit in various expressions and selects */
    __rename_walk_trigger(&s_walker, s_parse.new_trigger);
  }

  assert( rc==DBSQL_OK );
  rc = __rename_edit_sql(context, &s_ctx, sql, z_new, quote);

rename_column_func_done:
  if( rc!=DBSQL_OK ){
    if( rc==DBSQL_ERROR && __writable_schema(db) ){
      dbsql_result_value(context, argv[0]);
    }else if( s_parse.errmsg ){
      __rename_column_parse_error(context, "", argv[1], argv[2], &s_parse);
    }else{
      dbsql_result_error_code(context, rc);
    }
  }

  __rename_parse_cleanup(&s_parse);
  __rename_token_free(db, s_ctx.list);
#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = auth;
#endif
  __sm_leave_all(db);
}

/*
** walker_t expression callback used by "RENAME TABLE".
*/
static int __rename_table_expr_cb(walker_t *walker, expr_t *expr){
  rename_ctx_t *p = walker->u.rename;
  if( expr->op==TK_COLUMN
   && ALWAYS(EXPR_USE_Y_TAB(expr))
   && p->tab==expr->y.tab
  ){
    __rename_token_find(walker->parser, p, (void*)&expr->y.tab);
  }
  return WRC_Continue;
}

/*
** walker_t select callback used by "RENAME TABLE".
*/
static int __rename_table_select_cb(walker_t *walker, select_t *p_select){
  int i;
  rename_ctx_t *p = walker->u.rename;
  src_list_t *src = p_select->src;
  if( p_select->sel_flags & (SF_View|SF_CopyCte) ){
    testcase( p_select->sel_flags & SF_View );
    testcase( p_select->sel_flags & SF_CopyCte );
    return WRC_Prune;
  }
  if( NEVER(src==0) ){
    assert( walker->parser->db->malloc_failed );
    return WRC_Abort;
  }
  for(i=0; i<src->n_src; i++){
    src_item_t *item = &src->a[i];
    if( item->s_tab==p->tab ){
      __rename_token_find(walker->parser, p, item->name);
    }
  }
  __rename_walk_with(walker, p_select);

  return WRC_Continue;
}


/*
** This C function implements an SQL user function that is used by SQL code
** generated by the ALTER TABLE ... RENAME command to modify the definition
** of any foreign key constraints that use the table being renamed as the
** parent table. It is passed three arguments:
**
**   0: The database containing the table being renamed.
**   1. type:     Type of object ("table", "view" etc.)
**   2. object:   Name of object
**   3: The complete text of the schema statement being modified,
**   4: The old name of the table being renamed, and
**   5: The new name of the table being renamed.
**   6: True if the schema statement comes from the temp db.
**
** It returns the new schema statement. For example:
**
** dbsql_rename_table('main', 'CREATE TABLE t1(a REFERENCES t2)','t2','t3',0)
**       -> 'CREATE TABLE t1(a REFERENCES t3)'
*/
static void __rename_table_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  const char *z_db = (const char*)dbsql_value_text(argv[0]);
  const char *input = (const char*)dbsql_value_text(argv[3]);
  const char *old = (const char*)dbsql_value_text(argv[4]);
  const char *z_new = (const char*)dbsql_value_text(argv[5]);
  int temp = dbsql_value_int(argv[6]);
  UNUSED_PARAMETER(__not_used);

  if( input && old && z_new ){
    parser_t s_parse;
    int rc;
    int quote = 1;
    rename_ctx_t s_ctx;
    walker_t s_walker;

#ifndef DBSQL_OMIT_AUTHORIZATION
    dbsql_xauth auth = db->auth;
    db->auth = 0;
#endif

    __sm_enter_all(db);

    memset(&s_ctx, 0, sizeof(rename_ctx_t));
    s_ctx.tab = __find_table(db, old, z_db);
    memset(&s_walker, 0, sizeof(walker_t));
    s_walker.parser = &s_parse;
    s_walker.expr_callback = __rename_table_expr_cb;
    s_walker.select_callback = __rename_table_select_cb;
    s_walker.u.rename = &s_ctx;

    rc = __rename_parse_sql(&s_parse, z_db, db, input, temp);

    if( rc==DBSQL_OK ){
      int is_legacy = (db->flags & DBSQL_LegacyAlter);
      if( s_parse.new_table ){
        table_t *tab = s_parse.new_table;

        if( IsView(tab) ){
          if( is_legacy==0 ){
            select_t *p_select = tab->u.view.p_select;
            name_context_t s_nc;
            memset(&s_nc, 0, sizeof(s_nc));
            s_nc.parser = &s_parse;

            assert( p_select->sel_flags & SF_View );
            p_select->sel_flags &= ~(u_int32_t)SF_View;
            __select_prep(&s_parse, tab->u.view.p_select, &s_nc);
            if( s_parse.n_err ){
              rc = s_parse.rc;
            }else{
              __walk_select(&s_walker, tab->u.view.p_select);
            }
          }
        }else{
          /* Modify any FK definitions to point to the new table. */
#ifndef DBSQL_OMIT_FOREIGN_KEY
          if( (is_legacy==0 || (db->flags & DBSQL_ForeignKeys))
           && !IS_VIRTUAL(tab)
          ){
            f_key_t *f_key;
            assert( IS_ORDINARY_TABLE(tab) );
            for(f_key=tab->u.tab.f_key; f_key; f_key=f_key->next_from){
              if( dbsql_stricmp(f_key->to, old)==0 ){
                __rename_token_find(&s_parse, &s_ctx, (void*)f_key->to);
              }
            }
          }
#endif

          /* If this is the table being altered, fix any table refs in CHECK
          ** expressions. Also update the name that appears right after the
          ** "CREATE [VIRTUAL] TABLE" bit. */
          if( dbsql_stricmp(old, tab->name)==0 ){
            s_ctx.tab = tab;
            if( is_legacy==0 ){
              __walk_expr_list(&s_walker, tab->check);
            }
            __rename_token_find(&s_parse, &s_ctx, tab->name);
          }
        }
      }

      else if( s_parse.new_index ){
        __rename_token_find(&s_parse, &s_ctx, s_parse.new_index->name);
        if( is_legacy==0 ){
          __walk_expr(&s_walker, s_parse.new_index->part_idx_where);
        }
      }

#ifndef DBSQL_OMIT_TRIGGER
      else{
        trigger_t *P_TRIGGER = s_parse.new_trigger;
        trigger_step_t *step;
        if( 0==dbsql_stricmp(s_parse.new_trigger->table, old)
            && s_ctx.tab->schema==P_TRIGGER->tab_schema
          ){
          __rename_token_find(&s_parse, &s_ctx, s_parse.new_trigger->table);
        }

        if( is_legacy==0 ){
          rc = __rename_resolve_trigger(&s_parse);
          if( rc==DBSQL_OK ){
            __rename_walk_trigger(&s_walker, P_TRIGGER);
            for(step=P_TRIGGER->step_list; step; step=step->next){
              if( step->src ){
                int i;
                for(i=0; i<step->src->n_src; i++){
                  src_item_t *item = &step->src->a[i];
                  if( 0==dbsql_stricmp(item->name, old) ){
                    __rename_token_find(&s_parse, &s_ctx, item->name);
                  }
                }
              }
            }
          }
        }
      }
#endif
    }

    if( rc==DBSQL_OK ){
      rc = __rename_edit_sql(context, &s_ctx, input, z_new, quote);
    }
    if( rc!=DBSQL_OK ){
      if( rc==DBSQL_ERROR && __writable_schema(db) ){
        dbsql_result_value(context, argv[3]);
      }else if( s_parse.errmsg ){
        __rename_column_parse_error(context, "", argv[1], argv[2], &s_parse);
      }else{
        dbsql_result_error_code(context, rc);
      }
    }

    __rename_parse_cleanup(&s_parse);
    __rename_token_free(db, s_ctx.list);
    __sm_leave_all(db);
#ifndef DBSQL_OMIT_AUTHORIZATION
    db->auth = auth;
#endif
  }

  return;
}

static int __rename_quotefix_expr_cb(walker_t *walker, expr_t *expr){
  if( expr->op==TK_STRING && (expr->flags & EP_DblQuoted) ){
    __rename_token_find(walker->parser, walker->u.rename, (const void*)expr);
  }
  return WRC_Continue;
}

/* SQL function: dbsql_rename_quotefix(DB,SQL)
**
** Rewrite the DDL statement "SQL" so that any string literals that use
** double-quotes use single quotes instead.
**
** Two arguments must be passed:
**
**   0: Database name ("main", "temp" etc.).
**   1: SQL statement to edit.
**
** The returned value is the modified SQL statement. For example, given
** the database schema:
**
**   CREATE TABLE t1(a, b, c);
**
**   SELECT dbsql_rename_quotefix('main',
**       'CREATE VIEW v1 AS SELECT "a", "string" FROM t1'
**   );
**
** returns the string:
**
**   CREATE VIEW v1 AS SELECT "a", 'string' FROM t1
**
** If there is a error in the input SQL, then raise an error, except
** if PRAGMA writable_schema=ON, then just return the input string
** unmodified following an error.
*/
static void __rename_quotefix_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  char const *z_db = (const char*)dbsql_value_text(argv[0]);
  char const *input = (const char*)dbsql_value_text(argv[1]);

#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth = db->auth;
  db->auth = 0;
#endif

  __sm_enter_all(db);

  UNUSED_PARAMETER(__not_used);
  if( z_db && input ){
    int rc;
    parser_t s_parse;
    rc = __rename_parse_sql(&s_parse, z_db, db, input, 0);

    if( rc==DBSQL_OK ){
      rename_ctx_t s_ctx;
      walker_t s_walker;

      /* walker_t to find tokens that need to be replaced. */
      memset(&s_ctx, 0, sizeof(rename_ctx_t));
      memset(&s_walker, 0, sizeof(walker_t));
      s_walker.parser = &s_parse;
      s_walker.expr_callback = __rename_quotefix_expr_cb;
      s_walker.select_callback = __rename_column_select_cb;
      s_walker.u.rename = &s_ctx;

      if( s_parse.new_table ){
        if( IsView(s_parse.new_table) ){
          select_t *p_select = s_parse.new_table->u.view.p_select;
          p_select->sel_flags &= ~(u_int32_t)SF_View;
          s_parse.rc = DBSQL_OK;
          __select_prep(&s_parse, p_select, 0);
          rc = (db->malloc_failed ? DBSQL_NOMEM : s_parse.rc);
          if( rc==DBSQL_OK ){
            __walk_select(&s_walker, p_select);
          }
        }else{
          int i;
          __walk_expr_list(&s_walker, s_parse.new_table->check);
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
          for(i=0; i<s_parse.new_table->n_col; i++){
            __walk_expr(&s_walker,
               __column_expr(s_parse.new_table,
                                         &s_parse.new_table->cols[i]));
          }
#endif /* DBSQL_OMIT_GENERATED_COLUMNS */
        }
      }else if( s_parse.new_index ){
        __walk_expr_list(&s_walker, s_parse.new_index->col_exprs);
        __walk_expr(&s_walker, s_parse.new_index->part_idx_where);
      }else{
#ifndef DBSQL_OMIT_TRIGGER
        rc = __rename_resolve_trigger(&s_parse);
        if( rc==DBSQL_OK ){
          __rename_walk_trigger(&s_walker, s_parse.new_trigger);
        }
#endif /* DBSQL_OMIT_TRIGGER */
      }

      if( rc==DBSQL_OK ){
        rc = __rename_edit_sql(context, &s_ctx, input, 0, 0);
      }
      __rename_token_free(db, s_ctx.list);
    }
    if( rc!=DBSQL_OK ){
      if( __writable_schema(db) && rc==DBSQL_ERROR ){
        dbsql_result_value(context, argv[1]);
      }else{
        dbsql_result_error_code(context, rc);
      }
    }
    __rename_parse_cleanup(&s_parse);
  }

#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = auth;
#endif

  __sm_leave_all(db);
}

/* Function:  dbsql_rename_test(DB,SQL,TYPE,NAME,ISTEMP,WHEN,DQS)
**
** An SQL user function that checks that there are no parse or symbol
** resolution problems in a CREATE TRIGGER|TABLE|VIEW|INDEX statement.
** After an ALTER TABLE .. RENAME operation is performed and the schema
** reloaded, this function is called on each SQL statement in the schema
** to ensure that it is still usable.
**
**   0: Database name ("main", "temp" etc.).
**   1: SQL statement.
**   2: Object type ("view", "table", "trigger" or "index").
**   3: Object name.
**   4: True if object is from temp schema.
**   5: "when" part of error message.
**   6: True to disable the DQS quirk when parsing SQL.
**
** The return value is computed as follows:
**
**   A. If an error is seen and not in PRAGMA writable_schema=ON mode,
**      then raise the error.
**   B. Else if a trigger is created and the the table that the trigger is
**      attached to is in database db, then return 1.
**   C. Otherwise return NULL.
*/
static void __rename_table_test(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  char const *z_db = (const char*)dbsql_value_text(argv[0]);
  char const *input = (const char*)dbsql_value_text(argv[1]);
  int temp = dbsql_value_int(argv[4]);
  int is_legacy = (db->flags & DBSQL_LegacyAlter);
  char const *when = (const char*)dbsql_value_text(argv[5]);
  int no_dqs = dbsql_value_int(argv[6]);

#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth = db->auth;
  db->auth = 0;
#endif

  UNUSED_PARAMETER(__not_used);

  if( z_db && input ){
    int rc;
    parser_t s_parse;
    u_int64_t flags = db->flags;
    if( no_dqs ) db->flags &= ~(DBSQL_DqsDML|DBSQL_DqsDDL);
    rc = __rename_parse_sql(&s_parse, z_db, db, input, temp);
    db->flags = flags;
    if( rc==DBSQL_OK ){
      if( is_legacy==0 && s_parse.new_table && IsView(s_parse.new_table) ){
        name_context_t s_nc;
        memset(&s_nc, 0, sizeof(s_nc));
        s_nc.parser = &s_parse;
        __select_prep(&s_parse, s_parse.new_table->u.view.p_select, &s_nc);
        if( s_parse.n_err ) rc = s_parse.rc;
      }

      else if( s_parse.new_trigger ){
        if( is_legacy==0 ){
          rc = __rename_resolve_trigger(&s_parse);
        }
        if( rc==DBSQL_OK ){
          int i1 = __schema_to_index(db, s_parse.new_trigger->tab_schema);
          int i2 = __find_db_name(db, z_db);
          if( i1==i2 ){
            /* Handle output case B */
            dbsql_result_int(context, 1);
          }
        }
      }
    }

    if( rc!=DBSQL_OK && when && !__writable_schema(db) ){
      /* Output case A */
      __rename_column_parse_error(context, when, argv[2], argv[3],&s_parse);
    }
    __rename_parse_cleanup(&s_parse);
  }

#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = auth;
#endif
}


/*
** Return the number of bytes until the end of the next non-whitespace and
** non-comment token.  For the purpose of this function, a "(" token includes
** all of the bytes through and including the matching ")", or until the
** first illegal token, whichever comes first.
**
** Write the token type into *pi_token.
**
** The value returned is the number of bytes in the token itself plus
** the number of bytes of leading whitespace and comments skipped plus
** all bytes through the next matching ")" if the token is TK_LP.
**
** Example:    (Note: '.' used in place of '*' in the example z[] text)
**
**                                    ,--------- *pi_token := TK_RP
**                                    v
**    z[] = " /.comment./ --comment\n (two three four) five"
**          |                                        |
**          |<-------------------------------------->|
**                              |
**                              `--- return value
*/
static int __get_constraint_token(const u_int8_t *z, int *pi_token){
  int i_off = 0;
  int t = 0;
  do {
    i_off += __get_token(&z[i_off], &t);
  }while( t==TK_SPACE || t==TK_COMMENT );

  *pi_token = t;

  if( t==TK_LP ){
    int n_nest = 1;
    while( n_nest>0 ){
      i_off += __get_token(&z[i_off], &t);
      if( t==TK_LP ){
        n_nest++;
      }else if( t==TK_RP ){
        t = TK_LP;
        n_nest--;
      }else if( t==TK_ILLEGAL ){
        break;
      }
    }
  }

  *pi_token = t;
  return i_off;
}

/*
** The implementation of internal UDF dbsql_drop_column().
**
** Arguments:
**
**  argv[0]: An integer - the index of the schema containing the table
**  argv[1]: CREATE TABLE statement to modify.
**  argv[2]: An integer - the index of the column to remove.
**
** The value returned is a string containing the CREATE TABLE statement
** with column argv[2] removed.
*/
static void __drop_column_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  int i_schema = dbsql_value_int(argv[0]);
  const char *sql = (const char*)dbsql_value_text(argv[1]);
  int i_col = dbsql_value_int(argv[2]);
  const char *z_db = db->dbs[i_schema].db_s_name;
  int rc;
  parser_t s_parse;
  rename_token_t *col;
  table_t *tab;
  const char *z_end;
  char *z_new = 0;

#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth = db->auth;
  db->auth = 0;
#endif

  UNUSED_PARAMETER(__not_used);
  rc = __rename_parse_sql(&s_parse, z_db, db, sql, i_schema==1);
  if( rc!=DBSQL_OK ) goto drop_column_done;
  tab = s_parse.new_table;
  if( tab==0 || tab->n_col==1 || i_col>=tab->n_col ){
    /* This can happen if the dbsql_schema table is corrupt */
    rc = DBSQL_CORRUPT_BKPT;
    goto drop_column_done;
  }

  if( i_col<tab->n_col-1 ){
    rename_token_t *end;
    col = __rename_token_find(&s_parse, 0, (void*)tab->cols[i_col].cn_name);
    end = __rename_token_find(&s_parse, 0, (void*)tab->cols[i_col+1].cn_name);
    z_end = (const char*)end->t.z;
  }else{
    int tok;
    assert( IS_ORDINARY_TABLE(tab) );
    assert( i_col!=0 );
    /* Point col->t.z at the "," immediately preceding the definition of
    ** the column being dropped. To do this, start at the name of the 
    ** previous column, and tokenize until the next ",".  */
    col = __rename_token_find(&s_parse, 0, (void*)tab->cols[i_col-1].cn_name);
    do {
      col->t.z += __get_constraint_token((const u_int8_t*)col->t.z, &tok);
    }while( tok!=TK_COMMA );
    col->t.z--;
    z_end = (const char*)&sql[tab->u.tab.add_col_offset];
  }

  z_new = __m_printf(db, "%.*s%s", col->t.z-sql, sql, z_end);
  dbsql_result_text(context, z_new, -1, DBSQL_TRANSIENT);
  dbsql_free(z_new);

drop_column_done:
  __rename_parse_cleanup(&s_parse);
#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = auth;
#endif
  if( rc!=DBSQL_OK ){
    dbsql_result_error_code(context, rc);
  }
}

/*
** This function is called by the parser upon parsing an
**
**     ALTER TABLE src DROP COLUMN name
**
** statement. Argument src contains the possibly qualified name of the
** table being edited, and token name the name of the column to drop.
*/
void __alter_drop_column(parser_t *parser, src_list_t *src, const Token *name){
  DBSQL *db = parser->db;       /* Database handle */
  table_t *tab;                    /* table_t to modify */
  int i_db;                        /* index_t of db containing tab in dbs[] */
  const char *z_db;                /* Database containing tab ("main" etc.) */
  char *col = 0;                 /* Name of column to drop */
  int i_col;                       /* index_t of column col in tab->cols[] */

  /* Look up the table being altered. */
  assert( parser->new_table==0 );
  assert( __sm_holds_all_mutexes(db) );
  if( NEVER(db->malloc_failed) ) goto exit_drop_column;
  tab = __locate_table_item(parser, 0, &src->a[0]);
  if( !tab ) goto exit_drop_column;

  /* Make sure this is not an attempt to ALTER a view, virtual table or
  ** system table. */
  if( DBSQL_OK!=__is_alterable_table(parser, tab) ) goto exit_drop_column;
  if( DBSQL_OK!=IS_REAL_TABLE(parser, tab, 1) ) goto exit_drop_column;

  /* Find the index of the column being dropped. */
  col = __name_from_token(db, name);
  if( col==0 ){
    assert( db->malloc_failed );
    goto exit_drop_column;
  }
  i_col = __column_index(tab, col);
  if( i_col<0 ){
    __error_msg(parser, "no such column: \"%T\"", name);
    goto exit_drop_column;
  }

  /* Do not allow the user to drop a PRIMARY KEY column or a column
  ** constrained by a UNIQUE constraint.  */
  if( tab->cols[i_col].col_flags & (COLFLAG_PRIMKEY|COLFLAG_UNIQUE) ){
    __error_msg(parser, "cannot drop %s column: \"%s\"",
        (tab->cols[i_col].col_flags&COLFLAG_PRIMKEY) ? "PRIMARY KEY" : "UNIQUE",
        col
    );
    goto exit_drop_column;
  }

  /* Do not allow the number of columns to go to zero */
  if( tab->n_col<=1 ){
    __error_msg(parser, "cannot drop column \"%s\": no other columns exist",col);
    goto exit_drop_column;
  }

  /* Edit the dbsql_schema table */
  i_db = __schema_to_index(db, tab->schema);
  assert( i_db>=0 );
  z_db = db->dbs[i_db].db_s_name;
#ifndef DBSQL_OMIT_AUTHORIZATION
  /* Invoke the authorization callback. */
  if( __auth_check(parser, DBSQL_ALTER_TABLE, z_db, tab->name, col) ){
    goto exit_drop_column;
  }
#endif
  __rename_test_schema(parser, z_db, i_db==1, "", 0);
  __rename_fix_quotes(parser, z_db, i_db==1);
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_drop_column(%d, sql, %d) "
      "WHERE (type=='table' AND tbl_name=%Q COLLATE nocase)"
      , z_db, i_db, i_col, tab->name
  );

  /* Drop and reload the database schema. */
  __rename_reload_schema(parser, i_db, INITFLAG_AlterDrop);
  __rename_test_schema(parser, z_db, i_db==1, "after drop column", 1);

  /* Edit rows of table on disk */
  if( parser->n_err==0 && (tab->cols[i_col].col_flags & COLFLAG_VIRTUAL)==0 ){
    int i;
    int addr;
    int reg;
    int reg_rec;
    index_t *pk = 0;
    int n_field = 0;               /* Number of non-virtual columns after drop */
    int i_cur;
    vdbe_t *v = __get_vdbe(parser);
    i_cur = parser->n_tab++;
    __open_table(parser, i_cur, i_db, tab, OP_OpenWrite);
    addr = __vdbe_add_op1(v, OP_Rewind, i_cur); VDBE_COVERAGE(v);
    reg = ++parser->n_mem;
    if( HAS_ROWID(tab) ){
      __vdbe_add_op2(v, OP_Rowid, i_cur, reg);
      parser->n_mem += tab->n_col;
    }else{
      pk = __primary_key_index(tab);
      parser->n_mem += pk->n_column;
      for(i=0; i<pk->n_key_col; i++){
        __vdbe_add_op3(v, OP_Column, i_cur, i, reg+i+1);
      }
      n_field = pk->n_key_col;
    }
    reg_rec = ++parser->n_mem;
    for(i=0; i<tab->n_col; i++){
      if( i!=i_col && (tab->cols[i].col_flags & COLFLAG_VIRTUAL)==0 ){
        int reg_out;
        if( pk ){
          int i_pos = __table_column_to_index(pk, i);
          int i_col_pos = __table_column_to_index(pk, i_col);
          if( i_pos<pk->n_key_col ) continue;
          reg_out = reg+1+i_pos-(i_pos>i_col_pos);
        }else{
          reg_out = reg+1+n_field;
        }
        if( i==tab->i_p_key ){
          __vdbe_add_op2(v, OP_Null, 0, reg_out);
        }else{
          char aff = tab->cols[i].affinity;
          if( aff==DBSQL_AFF_REAL ){
            tab->cols[i].affinity = DBSQL_AFF_NUMERIC;
          }
          __expr_code_get_column_of_table(v, tab, i_cur, i, reg_out);
          tab->cols[i].affinity = aff;
        }
        n_field++;
      }
    }
    if( n_field==0 ){
      /* dbsqlfuzz 5f09e7bcc78b4954d06bf9f2400d7715f48d1fef */
      parser->n_mem++;
      __vdbe_add_op2(v, OP_Null, 0, reg+1);
      n_field = 1;
    }
    __vdbe_add_op3(v, OP_MakeRecord, reg+1, n_field, reg_rec);
    if( pk ){
      __vdbe_add_op4_int(v, OP_IdxInsert, i_cur, reg_rec, reg+1, pk->n_key_col);
    }else{
      __vdbe_add_op3(v, OP_Insert, i_cur, reg_rec, reg);
    }
    __vdbe_change_p5(v, OPFLAG_SAVEPOSITION);

    __vdbe_add_op2(v, OP_Next, i_cur, addr+1); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr);
  }

exit_drop_column:
  __dbsql_db_free(db, col);
  __src_list_delete(db, src);
}

/*
** Return the number of bytes of leading whitespace/comments in string z[].
*/
static int __get_whitespace(const u_int8_t *z){
  int n_ret = 0;
  while( 1 ){
    int t = 0;
    int n = __get_token(&z[n_ret], &t);
    if( t!=TK_SPACE && t!=TK_COMMENT ) break;
    n_ret += n;
  }
  return n_ret;
}


/*
** Argument z points into the body of a constraint - specifically the 
** second token of the constraint definition.  For a named constraint,
** z points to the first token past the CONSTRAINT keyword.  For an
** unnamed NOT NULL constraint, z points to the first byte past the NOT
** keyword.
**
** Return the number of bytes until the end of the constraint. 
*/
static int __get_constraint(const u_int8_t *z){
  int i_off = 0;
  int t = 0;

  /* Now, the current constraint proceeds until the next occurence of one 
  ** of the following tokens: 
  **
  **   CONSTRAINT, PRIMARY, NOT, UNIQUE, CHECK, DEFAULT, 
  **   COLLATE, REFERENCES, FOREIGN, GENERATED, AS, RP, or COMMA
  **
  ** Also exit the loop if ILLEGAL turns up.
  */
  while( 1 ){
    int n = __get_constraint_token(&z[i_off], &t);
    if( t==TK_CONSTRAINT || t==TK_PRIMARY || t==TK_NOT || t==TK_UNIQUE
     || t==TK_CHECK || t==TK_DEFAULT || t==TK_COLLATE || t==TK_REFERENCES
     || t==TK_FOREIGN || t==TK_RP || t==TK_COMMA || t==TK_ILLEGAL
     || t==TK_AS || t==TK_GENERATED
    ){
      break;
    }
    i_off += n;
  }
  
  return i_off;
}

/*
** Compare two constraint names.
**
** Summary:   *res := quote != cmp
**
** Details:
** Compare the (possibly quoted) constraint name quote[0..n_quote-1]
** against cmp[].  Write zero into *res if they are the same and
** non-zero if they differ.  Normally return DBSQL_OK, except if there
** is an OOM, set the OOM error condition on ctx and return DBSQL_NOMEM.
*/
static int __quoted_compare(
  dbsql_func_t *ctx,  /* Function context on which to report errors */
  int t,                 /* Token type */
  const u_int8_t *quote,      /* Possibly quoted text.  Not zero-terminated. */
  int n_quote,            /* Length of quote in bytes */
  const u_int8_t *cmp,        /* Zero-terminated, unquoted name to compare against */
  int *res              /* OUT: Set to 0 if equal, non-zero if unequal */
){
  char *copy = 0;       /* De-quoted, zero-terminated copy of quote[] */

  if( t==TK_ILLEGAL ){
    *res = 1;
    return DBSQL_OK;
  }
  copy = __malloc_zero(n_quote+1);
  if( copy==0 ){
    dbsql_result_error_nomem(ctx);
    return DBSQL_NOMEM_BKPT;
  }
  memcpy(copy, quote, n_quote);
  __dequote(copy);
  *res = dbsql_stricmp((const char*)copy, (const char*)cmp);
  dbsql_free(copy);
  return DBSQL_OK;
}

/*
** sql[] is a CREATE TABLE statement, supposedly.  Find the offset
** into sql[] of the first character past the first "(" and write
** that offset into *pi_off and return DBSQL_OK.  Or, if not found,
** set the DBSQL_CORRUPT error code and return DBSQL_ERROR.
*/
static int __skip_create_table(dbsql_func_t *ctx, const u_int8_t *sql, int *pi_off){
  int i_off = 0;

  if( sql==0 ) return DBSQL_ERROR;

  /* Jump past the "CREATE TABLE" bit. */
  while( 1 ){
    int t = 0;
    i_off += __get_token(&sql[i_off], &t);
    if( t==TK_LP ) break;
    if( t==TK_ILLEGAL ){
      dbsql_result_error_code(ctx, DBSQL_CORRUPT_BKPT);
      return DBSQL_ERROR;
    }
  }

  *pi_off = i_off;
  return DBSQL_OK;
}

/*
** Internal SQL function dbsql_drop_constraint():  Given an input
** CREATE TABLE statement, return a revised CREATE TABLE statement
** with a constraint removed.  Two forms, depending on the datatype
** of argv[2]:
**
**   dbsql_drop_constraint(SQL, INT)  -- Omit NOT NULL from the INT-th column
**   dbsql_drop_constraint(SQL, TEXT) -- OMIT constraint with name TEXT
**
** In the first case, the left-most column is 0.
*/
static void __drop_constraint_func(
  dbsql_func_t *ctx,
  int __not_used,
  dbsql_value_t **argv
){
  const u_int8_t *sql = dbsql_value_text(argv[0]);
  const u_int8_t *cons = 0;
  int i_not_null = -1;
  int ii;
  int i_off = 0;
  int i_start = 0;
  int i_end = 0;
  char *z_new = 0;
  int t = 0;
  DBSQL *db;
  UNUSED_PARAMETER(__not_used);

  if( sql==0 ) return;

  /* Jump past the "CREATE TABLE" bit. */
  if( __skip_create_table(ctx, sql, &i_off) ) return;

  if( dbsql_value_type(argv[1])==DBSQL_INTEGER ){
    i_not_null = dbsql_value_int(argv[1]);
  }else{
    cons = dbsql_value_text(argv[1]);
  }

  /* Search for the named constraint within column definitions. */
  for(ii=0; i_end==0; ii++){
  
    /* Now parse the column or table constraint definition. Search
    ** for the token CONSTRAINT if this is a DROP CONSTRAINT command, or
    ** NOT in the right column if this is a DROP NOT NULL. */
    while( 1 ){
      i_start = i_off;
      i_off += __get_constraint_token(&sql[i_off], &t);
      if( t==TK_CONSTRAINT && (cons || i_not_null==ii) ){
        /* Check if this is the constraint we are searching for. */
        int n_tok = 0;
        int cmp = 1;

        /* Skip past any whitespace. */
        i_off += __get_whitespace(&sql[i_off]);

        /* Compare the next token - which may be quoted - with the name of
        ** the constraint being dropped.  */
        n_tok = __get_constraint_token(&sql[i_off], &t);
        if( cons ){
          if( __quoted_compare(ctx, t, &sql[i_off], n_tok, cons, &cmp) ) return;
        }
        i_off += n_tok;

        /* The next token is usually the first token of the constraint
        ** definition. This is enough to tell the type of the constraint - 
        ** TK_NOT means it is a NOT NULL, TK_CHECK a CHECK constraint etc.
        **
        ** There is also the chance that the next token is TK_CONSTRAINT
        ** (or TK_DEFAULT or TK_COLLATE), for example if a table has been
        ** created as follows:
        **
        **    CREATE TABLE t1(cols, CONSTRAINT one CONSTRAINT two NOT NULL);
        **
        ** In this case, allow the "CONSTRAINT one" bit to be dropped by
        ** this command if that is what is requested, or to advance to
        ** the next iteration of the loop with &sql[i_off] still pointing
        ** to the CONSTRAINT keyword.  */
        n_tok = __get_constraint_token(&sql[i_off], &t);
        if( t==TK_CONSTRAINT || t==TK_DEFAULT || t==TK_COLLATE 
         || t==TK_COMMA || t==TK_RP || t==TK_GENERATED || t==TK_AS 
        ){
          t = TK_CHECK;
        }else{
          i_off += n_tok;
          i_off += __get_constraint(&sql[i_off]);
        }

        if( cmp==0 || (i_not_null>=0 && t==TK_NOT) ){
          if( t!=TK_NOT && t!=TK_CHECK ){
            __error_m_printf(ctx, "constraint may not be dropped: %s", cons);
            return;
          }
          i_end = i_off;
          break;
        }

      }else if( t==TK_NOT && i_not_null==ii ){
        i_end = i_off + __get_constraint(&sql[i_off]);
        break;
      }else if( t==TK_RP || t==TK_ILLEGAL ){
        i_end = -1;
        break;
      }else if( t==TK_COMMA ){
        break;
      }
    }
  }

  /* If the constraint has not been found it is an error. */
  if( i_end<=0 ){
    if( cons ){
      __error_m_printf(ctx, "no such constraint: %s", cons);
    }else{
      /* dbsql follows postgres in that a DROP NOT NULL on a column that is
      ** not NOT NULL is not an error. So just return the original SQL here. */
      dbsql_result_text(ctx, (const char*)sql, -1, DBSQL_TRANSIENT);
    }
  }else{

    /* Figure out if an extra space should be inserted after the constraint
    ** is removed. And if an additional comma preceding the constraint 
    ** should be removed. */
    const char *space = " ";
    i_end += __get_whitespace(&sql[i_end]);
    __get_token(&sql[i_end], &t);
    if( t==TK_RP || t==TK_COMMA ){
      space = "";
      if( sql[i_start-1]==',' ) i_start--;
    }

    db = dbsql_context_db_handle(ctx);
    z_new = __m_printf(db, "%.*s%s%s", i_start, sql, space, &sql[i_end]);
    dbsql_result_text(ctx, z_new, -1, DBSQL_DYNAMIC);
  }
}

/*
** Internal SQL function:
**
**     dbsql_add_constraint(SQL, CONSTRAINT-TEXT, ICOL)
**
** SQL is a CREATE TABLE statement.  Return a modified version of
** SQL that adds CONSTRAINT-TEXT at the end of the ICOL-th column
** definition.  (The left-most column defintion is 0.)
*/
static void __add_constraint_func(
  dbsql_func_t *ctx,
  int __not_used,
  dbsql_value_t **argv
){
  const u_int8_t *sql = dbsql_value_text(argv[0]);
  const char *cons = (const char*)dbsql_value_text(argv[1]);
  int i_col = dbsql_value_int(argv[2]);
  int i_off = 0;
  int ii;
  char *z_new = 0;
  int t = 0;
  DBSQL *db;
  UNUSED_PARAMETER(__not_used);

  if( __skip_create_table(ctx, sql, &i_off) ) return;
  
  for(ii=0; ii<=i_col || (i_col<0 && t!=TK_RP); ii++){
    i_off += __get_constraint_token(&sql[i_off], &t);
    while( 1 ){
      int n_tok = __get_constraint_token(&sql[i_off], &t);
      if( t==TK_COMMA || t==TK_RP ) break;
      if( t==TK_ILLEGAL ){
        dbsql_result_error_code(ctx, DBSQL_CORRUPT_BKPT);
        return;
      }
      i_off += n_tok;
    }
  }

  i_off += __get_whitespace(&sql[i_off]);

  db = dbsql_context_db_handle(ctx);
  if( i_col<0 ){
    z_new = __m_printf(db, "%.*s, %s%s", i_off, sql, cons, &sql[i_off]);
  }else{
    z_new = __m_printf(db, "%.*s %s%s", i_off, sql, cons, &sql[i_off]);
  }
  dbsql_result_text(ctx, z_new, -1, DBSQL_DYNAMIC);
}

/*
** Find a column named col in table tab. If successful, set output 
** parameter *pi_col to the index of the column in the table and return
** DBSQL_OK. Otherwise, set *pi_col to -1 and return an dbsql error
** code.
*/
static int __alter_find_col(parser_t *parser, table_t *tab, Token *col, int *pi_col){
  DBSQL *db = parser->db;
  char *name = __name_from_token(db, col);
  int rc = DBSQL_NOMEM;
  int i_col = -1;

  if( name ){
    i_col = __column_index(tab, name);
    if( i_col<0 ){
      __error_msg(parser, "no such column: %s", name);
      rc = DBSQL_ERROR;
    }else{
      rc = DBSQL_OK;
    }
  }

#ifndef DBSQL_OMIT_AUTHORIZATION
  if( rc==DBSQL_OK ){
    const char *z_db = db->dbs[__schema_to_index(db, tab->schema)].db_s_name;
    const char *z_col = tab->cols[i_col].cn_name;
    if( __auth_check(parser, DBSQL_ALTER_TABLE, z_db, tab->name, z_col) ){
      tab = 0;
    }
  }
#endif

  __dbsql_db_free(db, name);
  *pi_col = i_col;
  return rc;
}


/*
** Find the table named by the first entry in source list src. If successful,
** return a pointer to the table_t structure and set output variable (*db)
** to point to the name of the database containin the table (i.e. "main",
** "temp" or the name of an attached database). 
**
** If the table cannot be located, return NULL. The value of the two output
** parameters is undefined in this case.
*/
static table_t *__alter_find_table(
  parser_t *parser,        /* Parsing context */
  src_list_t *src,        /* Name of the table to look for */
  int *pi_db,            /* OUT: write the i_db here */
  const char **pz_db,    /* OUT: write name of schema here */
  int auth             /* Do ALTER TABLE authorization checks if true */
){
  DBSQL *db = parser->db;
  table_t *tab = 0;
  assert( __sm_holds_all_mutexes(db) );
  tab = __locate_table_item(parser, 0, &src->a[0]);
  if( tab ){
    int i_db = __schema_to_index(db, tab->schema);
    *pz_db = db->dbs[i_db].db_s_name;
    *pi_db = i_db;

    if( DBSQL_OK!=IS_REAL_TABLE(parser, tab, 2) 
     || DBSQL_OK!=__is_alterable_table(parser, tab) 
    ){
      tab = 0;
    }
  }
#ifndef DBSQL_OMIT_AUTHORIZATION
  if( tab && auth ){
    if( __auth_check(parser, DBSQL_ALTER_TABLE, *pz_db, tab->name, 0) ){
      tab = 0;
    }
  }
#endif
  __src_list_delete(db, src);
  return tab;
}

/*
** Generate bytecode for one of:
**
**  (1)   ALTER TABLE src DROP CONSTRAINT cons
**  (2)   ALTER TABLE src ALTER col DROP NOT NULL
**
** One of cons and col must be NULL and the other non-null.
*/
void __alter_drop_constraint(
  parser_t *parser,    /* Parsing context */
  src_list_t *src,    /* The table being altered */
  Token *cons,     /* Name of the constraint to drop */
  Token *col       /* Name of the column from which to remove the NOT NULL */
){
  DBSQL *db = parser->db;
  table_t *tab = 0;
  int i_db = 0;
  const char *z_db = 0;
  char *arg = 0;

  assert( (col==0)!=(cons==0) );
  assert( src->n_src==1 );
  tab = __alter_find_table(parser, src, &i_db, &z_db, cons!=0);
  if( !tab ) return;

  if( cons ){
    char *z = __name_from_token(db, cons);
    arg = __m_printf(db, "%Q", z);
    __dbsql_db_free(db, z);
  }else{
    int i_col;
    if( __alter_find_col(parser, tab, col, &i_col) ) return;
    arg = __m_printf(db, "%d", i_col);
  }

  /* Edit the SQL for the named table. */
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_drop_constraint(sql, %s) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , z_db, arg, tab->name
  );
  __dbsql_db_free(db, arg);

  /* Finally, reload the database schema. */
  __rename_reload_schema(parser, i_db, INITFLAG_AlterDropCons);
}

/*
** The implementation of SQL function dbsql_fail(MSG). This takes a single
** argument, and returns it as an error message with the error code set to
** DBSQL_CONSTRAINT.
*/
static void __fail_constraint_func(
  dbsql_func_t *ctx,
  int __not_used,
  dbsql_value_t **argv
){
  const char *text = (const char*)dbsql_value_text(argv[0]);
  int err = dbsql_value_int(argv[1]);
  (void)__not_used;
  dbsql_result_error(ctx, text, -1);
  dbsql_result_error_code(ctx, err);
}

/*
** Buffer cons, which is n_cons bytes in size, contains the text of a 
** NOT NULL or CHECK constraint that will be inserted into a CREATE TABLE
** statement. If successful, this function returns the size of the buffer in
** bytes not including any trailing whitespace or "--" style comments. Or,
** if an OOM occurs, it returns 0 and sets db->malloc_failed to true.
**
** C-style comments at the end are preserved.  "--" style comments are
** removed because the comment terminator might be \000, and we are about
** to insert the cons[] text into the middle of a larger string, and that
** will have the effect of removing the comment terminator and messing up
** the syntax.
*/
static int __alter_rtrim_constraint(
  DBSQL *db,                    /* used to record OOM error */
  const char *cons,              /* Buffer containing constraint */
  int n_cons                       /* Size of cons in bytes */
){
  u_int8_t *tmp = (u_int8_t*)__m_printf(db, "%.*s", n_cons, cons);
  int i_off = 0;
  int i_end = 0;

  if( tmp==0 ) return 0;

  while( 1 ){
    int t = 0;
    int n_token = __get_token(&tmp[i_off], &t);
    if( t==TK_ILLEGAL ) break;
    if( t!=TK_SPACE && (t!=TK_COMMENT || tmp[i_off]!='-') ){
      i_end = i_off+n_token;
    }
    i_off += n_token;
  }

  __dbsql_db_free(db, tmp);
  return i_end;
}

/*
** Prepare a statement of the form:
**
**   ALTER TABLE src ALTER col SET NOT NULL
*/
void __alter_set_not_null(
  parser_t *parser,   /* Parsing context */
  src_list_t *src,   /* Name of the table being altered */
  Token *col,     /* Name of the column to add a NOT NULL constraint to */
  Token *first    /* The NOT token of the NOT NULL constraint text */
){
  table_t *tab = 0;
  int i_col = 0;
  int i_db = 0;
  const char *db = 0;
  const char *cons = 0;
  int n_cons = 0;

  /* Look up the table being altered. */
  assert( src->n_src==1 );
  tab = __alter_find_table(parser, src, &i_db, &db, 0);
  if( !tab ) return;

  /* Find the column being altered. */
  if( __alter_find_col(parser, tab, col, &i_col) ){
    return;
  }

  /* Find the length in bytes of the constraint definition */
  cons = first->z;
  n_cons = __alter_rtrim_constraint(parser->db, cons, parser->s_last_token.z - cons);

  /* Search for a constraint violation. Throw an exception if one is found. */
  __nested_parse(parser,
      "SELECT dbsql_fail('constraint failed', %d) "
      "FROM %Q.%Q AS x WHERE x.%.*s IS NULL", 
      DBSQL_CONSTRAINT, db, tab->name, (int)col->n, col->z
  );

  /* Edit the SQL for the named table. */
  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_add_constraint(dbsql_drop_constraint(sql, %d), %.*Q, %d) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , db, i_col, n_cons, cons, i_col, tab->name
  );

  /* Finally, reload the database schema. */
  __rename_reload_schema(parser, i_db, INITFLAG_AlterDropCons);
}

/*
** Implementation of internal SQL function:
**
**     dbsql_find_constraint(SQL, CONSTRAINT-NAME)
**
** This function returns true if the SQL passed as the first argument is a
** CREATE TABLE that contains a constraint with the name CONSTRAINT-NAME,
** or false otherwise.
*/
static void __find_constraint_func(
  dbsql_func_t *ctx,
  int __not_used,
  dbsql_value_t **argv
){
  const u_int8_t *sql = 0;
  const u_int8_t *cons = 0;
  int i_off = 0;
  int t = 0;

  (void)__not_used;
  sql = dbsql_value_text(argv[0]);
  cons = dbsql_value_text(argv[1]);

  if( sql==0 || cons==0 ) return;
  while( t!=TK_LP && t!=TK_ILLEGAL ){
    i_off += __get_token(&sql[i_off], &t);
  }

  while( 1 ){
    i_off += __get_constraint_token(&sql[i_off], &t);
    if( t==TK_CONSTRAINT ){
      int n_tok = 0;
      int cmp = 0;
      i_off += __get_whitespace(&sql[i_off]);
      n_tok = __get_constraint_token(&sql[i_off], &t);
      if( __quoted_compare(ctx, t, &sql[i_off], n_tok, cons, &cmp) ) return;
      if( cmp==0 ){
        dbsql_result_int(ctx, 1);
        return;
      }
    }else if( t==TK_ILLEGAL ){
      break;
    }
  }

  dbsql_result_int(ctx, 0);
}

/*
** Generate bytecode to implement:
**
**    ALTER TABLE src ADD [CONSTRAINT name] CHECK(expr)
**
** Any "ON CONFLICT" text that occurs after the "CHECK(...)", up
** until parser->s_last_token, is included as part of the new constraint.
*/
void __alter_add_constraint(
  parser_t *parser,           /* parser_t context */
  src_list_t *src,           /* table_t to add constraint to */
  Token *first,           /* First token of new constraint */
  Token *name,            /* Name of new constraint. NULL if name omitted. */
  const char *z_expr,       /* Text of CHECK expression */
  int n_expr,               /* Size of expr in bytes */
  expr_t *expr              /* The parsed CHECK expression */
){ 
  table_t *tab = 0;         /* table_t identified by src */
  int i_db = 0;             /* Which schema does tab live in */
  const char *db = 0;     /* Name of the schema in which tab lives */
  const char *cons = 0;   /* Text of the constraint */
  int n_cons;               /* Bytes of text to use from cons[] */
  int rc;                  /* Result from error checking expr */

  /* Look up the table being altered. */
  assert( src->n_src==1 );
  tab = __alter_find_table(parser, src, &i_db, &db, 1);
  if( !tab ){
    __expr_delete(parser->db, expr);
    return;
  }

  /* Verify that the new CHECK constraint does not contain any
  ** internal-use-only function.  Forum post 2026-05-10T01:11:28Z
  */
  rc = __resolve_self_reference(parser, tab, NC_IsCheck, expr, 0);
  __expr_delete(parser->db, expr);
  if( rc ) return;

  /* If this new constraint has a name, check that it is not a duplicate of
  ** an existing constraint. It is an error if it is.  */
  if( name ){
    char *z_name = __name_from_token(parser->db, name);

    __nested_parse(parser,
        "SELECT dbsql_fail('constraint %q already exists', %d) "
        "FROM \"%w\"." LEGACY_SCHEMA_TABLE " "
        "WHERE type='table' AND tbl_name=%Q COLLATE nocase "
        "AND dbsql_find_constraint(sql, %Q)",
        z_name, DBSQL_ERROR, db, tab->name, z_name
    );
    __dbsql_db_free(parser->db, z_name);
  }

  /* Search for a constraint violation. Throw an exception if one is found. */
  __nested_parse(parser,
      "SELECT dbsql_fail('constraint failed', %d) "
      "FROM %Q.%Q WHERE (%.*s) IS NOT TRUE", 
      DBSQL_CONSTRAINT, db, tab->name, n_expr, z_expr
  );

  /* Edit the SQL for the named table. */
  cons = first->z;
  n_cons = __alter_rtrim_constraint(parser->db, cons, parser->s_last_token.z - cons);

  __nested_parse(parser,
      "UPDATE \"%w\"." LEGACY_SCHEMA_TABLE " SET "
      "sql = dbsql_add_constraint(sql, %.*Q, -1) "
      "WHERE type='table' AND tbl_name=%Q COLLATE nocase"
      , db, n_cons, cons, tab->name
  );

  /* Finally, reload the database schema. */
  __rename_reload_schema(parser, i_db, INITFLAG_AlterDropCons);
}

/*
** Register built-in functions used to help implement ALTER TABLE
*/
void __alter_functions(void){
  static func_def_t alter_table_funcs[] = {
    INTERNAL_FUNCTION(dbsql_rename_column,  9, __rename_column_func),
    INTERNAL_FUNCTION(dbsql_rename_table,   7, __rename_table_func),
    INTERNAL_FUNCTION(dbsql_rename_test,    7, __rename_table_test),
    INTERNAL_FUNCTION(dbsql_drop_column,    3, __drop_column_func),
    INTERNAL_FUNCTION(dbsql_rename_quotefix,2, __rename_quotefix_func),
    INTERNAL_FUNCTION(dbsql_drop_constraint,2, __drop_constraint_func),
    INTERNAL_FUNCTION(dbsql_fail,           2, __fail_constraint_func),
    INTERNAL_FUNCTION(dbsql_add_constraint, 3, __add_constraint_func),
    INTERNAL_FUNCTION(dbsql_find_constraint,2, __find_constraint_func),
  };
  __insert_builtin_funcs(alter_table_funcs, ARRAY_SIZE(alter_table_funcs));
}
#endif  /* DBSQL_ALTER_TABLE */
