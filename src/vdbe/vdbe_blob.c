/*
** 2007 May 1
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
** This file contains code used to implement incremental BLOB I/O.
*/

#include "dbsql_int.h"
#include "vdbe_int.h"

#ifndef DBSQL_OMIT_INCRBLOB

/*
** Valid dbsql_blob_t* handles point to incrblob_t structures.
*/
typedef struct incrblob_t incrblob_t;
struct incrblob_t {
  int n_byte;              /* Size of open blob, in bytes */
  int i_offset;            /* Byte offset of blob in cursor data */
  u_int16_t i_col;               /* table_t column this handle is open on */
  sm_cursor_t *csr;         /* Cursor pointing at blob row */
  dbsql_stmt_t *stmt;    /* Statement holding cursor open */
  DBSQL *db;            /* The associated database */
  char *z_db;              /* Database name */
  table_t *tab;            /* table_t object */
};


/*
** This function is used by both blob_open() and blob_reopen(). It seeks
** the b-tree cursor associated with blob handle p to point to row i_row.
** If successful, DBSQL_OK is returned and subsequent calls to
** dbsql_blob_read() or dbsql_blob_write() access the specified row.
**
** If an error occurs, or if the specified row does not exist or does not
** contain a value of type TEXT or BLOB in the column nominated when the
** blob handle was opened, then an error code is returned and *err_p may
** be set to point to a buffer containing an error message. It is the
** responsibility of the caller to free the error message buffer using
** __dbsql_db_free().
**
** If an error does occur, then the b-tree cursor is closed. All subsequent
** calls to dbsql_blob_read(), blob_write() or blob_reopen() will 
** immediately return DBSQL_ABORT.
*/
static int __blob_seek_to_row(incrblob_t *p, int64_t i_row, char **err_p){
  int rc;                         /* Error code */
  char *err = 0;                 /* Error message */
  vdbe_t *v = (vdbe_t *)p->stmt;

  /* Set the value of register r[1] in the SQL statement to integer i_row. 
  ** This is done directly as a performance optimization
  */
  __vdbe_mem_set_int64(&v->mems[1], i_row);

  /* If the statement has been run before (and is paused at the OP_ResultRow)
  ** then back it up to the point where it does the OP_NotExists.  This could
  ** have been down with an extra OP_Goto, but simply setting the program
  ** counter is faster. */
  if( v->pc>4 ){
    v->pc = 4;
    assert( v->ops[v->pc].opcode==OP_NotExists );
    rc = __vdbe_exec(v);
  }else{
    rc = dbsql_step(p->stmt);
  }
  if( rc==DBSQL_ROW ){
    vdbe_cursor_t *c = v->csrs[0];
    u_int32_t type;
    assert( c!=0 );
    assert( c->cur_type==CURTYPE_BTREE );
    type = c->n_hdr_parsed>p->i_col ? c->types[p->i_col] : 0;
    testcase( c->n_hdr_parsed==p->i_col );
    testcase( c->n_hdr_parsed==p->i_col+1 );
    if( type<12 ){
      err = __m_printf(p->db, "cannot open value of type %s",
          type==0?"null": type==7?"real": "integer"
      );
      rc = DBSQL_ERROR;
      dbsql_finalize(p->stmt);
      p->stmt = 0;
    }else{
      p->i_offset = c->types[p->i_col + c->n_field];
      p->n_byte = __vdbe_serial_type_len(type);
      p->csr =  c->uc.cursor;
      __sm_incrblob_cursor(p->csr);
    }
  }

  if( rc==DBSQL_ROW ){
    rc = DBSQL_OK;
  }else if( p->stmt ){
    rc = dbsql_finalize(p->stmt);
    p->stmt = 0;
    if( rc==DBSQL_OK ){
      err = __m_printf(p->db, "no such rowid: %lld", i_row);
      rc = DBSQL_ERROR;
    }else{
      err = __m_printf(p->db, "%s", dbsql_errmsg(p->db));
    }
  }

  assert( rc!=DBSQL_OK || err==0 );
  assert( rc!=DBSQL_ROW && rc!=DBSQL_DONE );

  *err_p = err;
  return rc;
}

/*
** Open a blob handle.
*/
int dbsql_blob_open(
  DBSQL* db,            /* The database connection */
  const char *z_db,        /* The attached database containing the blob */
  const char *table,     /* The table containing the blob */
  const char *column,    /* The column containing the blob */
  int64_t i_row,      /* The row containing the glob */
  int wr_flag,             /* True -> read/write access, false -> read-only */
  dbsql_blob_t **pp_blob   /* Handle for accessing the blob returned here */
){
  int n_attempt = 0;
  int i_col;               /* index_t of column in row-record */
  int rc = DBSQL_OK;
  char *err = 0;
  table_t *tab;
  incrblob_t *blob = 0;
  int i_db;
  parser_t s_parse;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( pp_blob==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  *pp_blob = 0;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || table==0 || column==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  wr_flag = !!wr_flag;                /* wr_flag = (wr_flag ? 1 : 0); */

  dbsql_mutex_enter(db->mutex);

  blob = (incrblob_t *)__db_malloc_zero(db, sizeof(incrblob_t));
  while(1){
    __parse_object_init(&s_parse,db);
    if( !blob ) goto blob_open_out;
    __dbsql_db_free(db, err);
    err = 0;

    __sm_enter_all(db);
    tab = __locate_table(&s_parse, 0, table, z_db);
    if( tab && IS_VIRTUAL(tab) ){
      tab = 0;
      __error_msg(&s_parse, "cannot open virtual table: %s", table);
    }
    if( tab && !HAS_ROWID(tab) ){
      tab = 0;
      __error_msg(&s_parse, "cannot open table without rowid: %s", table);
    }
    if( tab && (tab->tab_flags&TF_HasGenerated)!=0 ){
      tab = 0;
      __error_msg(&s_parse, "cannot open table with generated columns: %s",
                      table);
    }
#ifndef DBSQL_OMIT_VIEW
    if( tab && IsView(tab) ){
      tab = 0;
      __error_msg(&s_parse, "cannot open view: %s", table);
    }
#endif
    if( tab==0
     || ((i_db = __schema_to_index(db, tab->schema))==1 &&
         __open_temp_database(&s_parse))
    ){
      if( s_parse.errmsg ){
        __dbsql_db_free(db, err);
        err = s_parse.errmsg;
        s_parse.errmsg = 0;
      }
      rc = DBSQL_ERROR;
      __sm_leave_all(db);
      goto blob_open_out;
    }
    blob->tab = tab;
    blob->z_db = db->dbs[i_db].db_s_name;

    /* Now search tab for the exact column. */
    i_col = __column_index(tab, column);
    if( i_col<0 ){
      __dbsql_db_free(db, err);
      err = __m_printf(db, "no such column: \"%s\"", column);
      rc = DBSQL_ERROR;
      __sm_leave_all(db);
      goto blob_open_out;
    }

    /* If the value is being opened for writing, check that the
    ** column is not indexed, and that it is not part of a foreign key. 
    */
    if( wr_flag ){
      const char *fault = 0;
      index_t *idx;
#ifndef DBSQL_OMIT_FOREIGN_KEY
      if( db->flags&DBSQL_ForeignKeys ){
        /* Check that the column is not part of an FK child key definition. It
        ** is not necessary to check if it is part of a parent key, as parent
        ** key columns must be indexed. The check below will pick up this 
        ** case.  */
        f_key_t *f_key;
        assert( IS_ORDINARY_TABLE(tab) );
        for(f_key=tab->u.tab.f_key; f_key; f_key=f_key->next_from){
          int j;
          for(j=0; j<f_key->n_col; j++){
            if( f_key->cols[j].i_from==i_col ){
              fault = "foreign key";
            }
          }
        }
      }
#endif
      for(idx=tab->p_index; idx; idx=idx->next){
        int j;
        for(j=0; j<idx->n_key_col; j++){
          /* FIXME: Be smarter about indexes that use expressions */
          if( idx->columns[j]==i_col || idx->columns[j]==XN_EXPR ){
            fault = "indexed";
          }
        }
      }
      if( fault ){
        __dbsql_db_free(db, err);
        err = __m_printf(db, "cannot open %s column for writing", fault);
        rc = DBSQL_ERROR;
        __sm_leave_all(db);
        goto blob_open_out;
      }
    }

    blob->stmt = (dbsql_stmt_t *)__vdbe_create(&s_parse);
    assert( blob->stmt || db->malloc_failed );
    if( blob->stmt ){
      
      /* This VDBE program seeks a btree cursor to the identified 
      ** db/table/row entry. The reason for using a vdbe program instead
      ** of writing code to use the b-tree layer directly is that the
      ** vdbe program will take advantage of the various transaction,
      ** locking and error handling infrastructure built into the vdbe.
      **
      ** After seeking the cursor, the vdbe executes an OP_ResultRow.
      ** Code external to the vdbe_t then "borrows" the b-tree cursor and
      ** uses it to implement the blob_read(), blob_write() and 
      ** blob_bytes() functions.
      **
      ** The dbsql_blob_close() function finalizes the vdbe program,
      ** which closes the b-tree cursor and (possibly) commits the 
      ** transaction.
      */
      static const int i_ln = VDBE_OFFSET_LINENO(2);
      static const vdbe_op_list_t open_blob[] = {
        {OP_TableLock,      0, 0, 0},  /* 0: Acquire a read or write lock */
        {OP_OpenRead,       0, 0, 0},  /* 1: Open a cursor */
        /* __blob_seek_to_row() will initialize r[1] to the desired rowid */
        {OP_NotExists,      0, 5, 1},  /* 2: Seek the cursor to rowid=r[1] */
        {OP_Column,         0, 0, 1},  /* 3  */
        {OP_ResultRow,      1, 0, 0},  /* 4  */
        {OP_Halt,           0, 0, 0},  /* 5  */
      };
      vdbe_t *v = (vdbe_t *)blob->stmt;
      vdbe_op_t *ops;

      __vdbe_add_op4_int(v, OP_Transaction, i_db, wr_flag, 
                           tab->schema->schema_cookie,
                           tab->schema->i_generation);
      __vdbe_change_p5(v, 1);
      assert( __vdbe_current_addr(v)==2 || db->malloc_failed );
      ops = __vdbe_add_op_list(v, ARRAY_SIZE(open_blob), open_blob, i_ln);

      /* Make sure a mutex is held on the table to be accessed */
      __vdbe_uses_btree(v, i_db); 

      if( db->malloc_failed==0 ){
        assert( ops!=0 );
        /* Configure the OP_TableLock instruction */
#ifdef DBSQL_OMIT_SHARED_CACHE
        ops[0].opcode = OP_Noop;
#else
        ops[0].p1 = i_db;
        ops[0].p2 = tab->tnum;
        ops[0].p3 = wr_flag;
        __vdbe_change_p4(v, 2, tab->name, P4_TRANSIENT);
      }
      if( db->malloc_failed==0 ){
#endif

        /* Remove either the OP_OpenWrite or __open_read. Set the P2 
        ** parameter of the other to tab->tnum.  */
        if( wr_flag ) ops[1].opcode = OP_OpenWrite;
        ops[1].p2 = tab->tnum;
        ops[1].p3 = i_db;   

        /* Configure the number of columns. Configure the cursor to
        ** think that the table has one more column than it really
        ** does. An OP_Column to retrieve this imaginary column will
        ** always return an SQL NULL. This is useful because it means
        ** we can invoke OP_Column to fill in the vdbe cursors type 
        ** and offset cache without causing any IO.
        */
        ops[1].p4type = P4_INT32;
        ops[1].p4.i = tab->n_col+1;
        ops[3].p2 = tab->n_col;

        s_parse.n_var = 0;
        s_parse.n_mem = 1;
        s_parse.n_tab = 1;
        __vdbe_make_ready(v, &s_parse);
      }
    }
   
    blob->i_col = i_col;
    blob->db = db;
    __sm_leave_all(db);
    if( db->malloc_failed ){
      goto blob_open_out;
    }
    rc = __blob_seek_to_row(blob, i_row, &err);
    if( (++n_attempt)>=DBSQL_MAX_SCHEMA_RETRY || rc!=DBSQL_SCHEMA ) break;
    __parse_object_reset(&s_parse);
  }

blob_open_out:
  if( rc==DBSQL_OK && db->malloc_failed==0 ){
    *pp_blob = (dbsql_blob_t *)blob;
  }else{
    if( blob && blob->stmt ) __vdbe_finalize((vdbe_t *)blob->stmt);
    __dbsql_db_free(db, blob);
  }
  __error_with_msg(db, rc, (err ? "%s" : (char*)0), err);
  __dbsql_db_free(db, err);
  __parse_object_reset(&s_parse);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Close a blob handle that was previously created using
** dbsql_blob_open().
*/
int dbsql_blob_close(dbsql_blob_t *blob){
  incrblob_t *p = (incrblob_t *)blob;
  int rc;
  DBSQL *db;

  if( p ){
    dbsql_stmt_t *stmt = p->stmt;
    db = p->db;
    dbsql_mutex_enter(db->mutex);
    __dbsql_db_free(db, p);
    dbsql_mutex_leave(db->mutex);
    rc = dbsql_finalize(stmt);
  }else{
    rc = DBSQL_OK;
  }
  return rc;
}

/*
** Perform a read or write operation on a blob
*/
static int __blob_read_write(
  dbsql_blob_t *blob, 
  void *z, 
  int n, 
  int i_offset, 
  int (*call)(sm_cursor_t*, u_int32_t, u_int32_t, void*)
){
  int rc = DBSQL_OK;
  incrblob_t *p = (incrblob_t *)blob;
  vdbe_t *v;
  DBSQL *db;

  if( p==0 ) return DBSQL_MISUSE_BKPT;
  db = p->db;
  dbsql_mutex_enter(db->mutex);
  v = (vdbe_t*)p->stmt;

  if( n<0 || i_offset<0 || ((int64_t)i_offset+n)>p->n_byte ){
    /* Request is out of range. Return a transient error. */
    rc = DBSQL_ERROR;
  }else if( v==0 ){
    /* If there is no statement handle, then the blob-handle has
    ** already been invalidated. Return DBSQL_ABORT in this case.
    */
    rc = DBSQL_ABORT;
  }else{
    /* Call either __btree_data() or __btree_put_data(). If DBSQL_ABORT is
    ** returned, clean-up the statement handle.
    */
    assert( db == v->db );
    __sm_enter_cursor(p->csr);

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
    if( call==__sm_put_data && db->pre_update_callback ){
      /* If a pre-update hook is registered and this is a write cursor, 
      ** invoke it here. 
      ** 
      ** TODO: The preupdate-hook is passed DBSQL_DELETE, even though this
      ** operation should really be an DBSQL_UPDATE. This is probably
      ** incorrect, but is convenient because at this point the new.* values 
      ** are not easily obtainable. And for the sessions module, an 
      ** DBSQL_UPDATE where the PK columns do not change is handled in the 
      ** same way as an DBSQL_DELETE (the DBSQL_DELETE code is actually
      ** slightly more efficient). Since you cannot write to a PK column
      ** using the incremental-blob API, this works. For the sessions module
      ** anyhow.
      */
      if( __sm_cursor_is_valid_nn(p->csr)==0 ){
        /* If the cursor is not currently valid, try to reseek it. This 
        ** always either fails or finds the correct row - the cursor will
        ** have been marked permanently CURSOR_INVALID if the open row has
        ** been deleted.  */
        int diff = 0;
        rc = __sm_cursor_restore(p->csr, &diff);
        assert( diff==0 || __sm_cursor_is_valid_nn(p->csr)==0 );
      }
      if( __sm_cursor_is_valid_nn(p->csr) ){
        int64_t i_key;
        i_key = __sm_integer_key(p->csr);
        assert( v->csrs[0]!=0 );
        assert( v->csrs[0]->cur_type==CURTYPE_BTREE );
        __vdbe_pre_update_hook(
            v, v->csrs[0], DBSQL_DELETE, p->z_db, p->tab, i_key, -1, p->i_col
        );
      }
    }
    if( rc==DBSQL_OK ){
      rc = call(p->csr, i_offset+p->i_offset, n, z);
    }
#else
    rc = call(p->csr, i_offset+p->i_offset, n, z);
#endif

    __sm_leave_cursor(p->csr);
    if( rc==DBSQL_ABORT ){
      __vdbe_finalize(v);
      p->stmt = 0;
    }else{
      v->rc = rc;
    }
  }
  __dbsql_error(db, rc);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Read data from a blob handle.
*/
int dbsql_blob_read(dbsql_blob_t *blob, void *z, int n, int i_offset){
  return __blob_read_write(blob, z, n, i_offset, __sm_payload_checked);
}

/*
** Write data to a blob handle.
*/
int dbsql_blob_write(dbsql_blob_t *blob, const void *z, int n, int i_offset){
  return __blob_read_write(blob, (void *)z, n, i_offset, __sm_put_data);
}

/*
** Query a blob handle for the size of the data.
**
** The incrblob_t.n_byte field is fixed for the lifetime of the incrblob_t
** so no mutex is required for access.
*/
int dbsql_blob_bytes(dbsql_blob_t *blob){
  incrblob_t *p = (incrblob_t *)blob;
  return (p && p->stmt) ? p->n_byte : 0;
}

/*
** Move an existing blob handle to point to a different row of the same
** database table.
**
** If an error occurs, or if the specified row does not exist or does not
** contain a blob or text value, then an error code is returned and the
** database handle error code and message set. If this happens, then all 
** subsequent calls to dbsql_blob_xxx() functions (except blob_close()) 
** immediately return DBSQL_ABORT.
*/
int dbsql_blob_reopen(dbsql_blob_t *blob, int64_t i_row){
  int rc;
  incrblob_t *p = (incrblob_t *)blob;
  DBSQL *db;

  if( p==0 ) return DBSQL_MISUSE_BKPT;
  db = p->db;
  dbsql_mutex_enter(db->mutex);

  if( p->stmt==0 ){
    /* If there is no statement handle, then the blob-handle has
    ** already been invalidated. Return DBSQL_ABORT in this case.
    */
    rc = DBSQL_ABORT;
  }else{
    char *err;
    ((vdbe_t*)p->stmt)->rc = DBSQL_OK;
    rc = __blob_seek_to_row(p, i_row, &err);
    if( rc!=DBSQL_OK ){
      __error_with_msg(db, rc, (err ? "%s" : (char*)0), err);
      __dbsql_db_free(db, err);
    }
    assert( rc!=DBSQL_SCHEMA );
  }

  rc = __api_exit(db, rc);
  assert( rc==DBSQL_OK || p->stmt==0 );
  dbsql_mutex_leave(db->mutex);
  return rc;
}

#endif /* #ifndef DBSQL_OMIT_INCRBLOB */
