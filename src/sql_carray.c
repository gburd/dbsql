/*
** 2016-06-29
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
** This file implements a table-valued-function that
** returns the values in a C-language array.
** Examples:
**
**      SELECT * FROM carray($ptr,5)
**
** The query above returns 5 integers contained in a C-language array
** at the address $ptr.  $ptr is a pointer to the array of integers.
** The pointer value must be assigned to $ptr using the
** dbsql_bind_pointer() interface with a pointer type of "carray".
** For example:
**
**    static int xes[] = { 53, 9, 17, 2231, 4, 99 };
**    int i = dbsql_bind_parameter_index(stmt, "$ptr");
**    dbsql_bind_pointer(stmt, i, xes, "carray", 0);
**
** There is an optional third parameter to determine the datatype of
** the C-language array.  Allowed values of the third parameter are
** 'int32', 'int64', 'double', 'char*', 'struct iovec'.  Example:
**
**      SELECT * FROM carray($ptr,10,'char*');
**
** The default value of the third parameter is 'int32'.
**
** HOW IT WORKS
**
** The carray "function" is really a virtual table with the
** following schema:
**
**     CREATE TABLE carray(
**       value,
**       pointer HIDDEN,
**       count HIDDEN,
**       ctype TEXT HIDDEN
**     );
**
** If the hidden columns "pointer" and "count" are unconstrained, then 
** the virtual table has no rows.  Otherwise, the virtual table interprets
** the integer value of "pointer" as a pointer to the array and "count"
** as the number of elements in the array.  The virtual table steps through
** the array, element by element.
*/
#if !defined(DBSQL_OMIT_VIRTUALTABLE) && defined(DBSQL_ENABLE_CARRAY)
#include "dbsql_int.h"
#if defined(_WIN32) || defined(__RTP__) || defined(_WRS_KERNEL)
  struct iovec {
    void *iov_base;
    size_t iov_len;
  };
#else
# include <sys/uio.h>
#endif

/*
** Names of allowed datatypes
*/
static const char *__az_carray_type[] = {
  "int32", "int64", "double", "char*", "struct iovec"
};

/*
** Structure used to hold the dbsql_carray_bind() information
*/
typedef struct carray_bind carray_bind;
struct carray_bind {
  void *data;                /* The data */
  int n_data;                  /* Number of elements */
  int m_flags;                 /* Control flags */
  void (*x_del)(void*);        /* Destructor for data */
  void *del;                 /* Alternative argument to del() */
};


/* carray_cursor is a subclass of dbsql_vtab_cursor_t which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct carray_cursor carray_cursor;
struct carray_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_rowid;      /* The rowid */
  void *ptr;                /* Pointer to the array of values */
  int64_t i_cnt;        /* Number of integers in the array */
  unsigned char type;       /* One of the CARRAY_type values */
};

/*
** The __carray_connect() method is invoked to create a new
** carray_vtab that describes the carray virtual table.
**
** Think of this routine as the constructor for carray_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the carray_vtab object and initialize all fields.
**
**    (2) Tell dbsql (via the dbsql_declare_vtab() interface) what the
**        result set of queries against carray will look like.
*/
static int __carray_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  dbsql_vtab_t *p_new;
  int rc;

/* column_t numbers */
#define CARRAY_COLUMN_VALUE   0
#define CARRAY_COLUMN_POINTER 1
#define CARRAY_COLUMN_COUNT   2
#define CARRAY_COLUMN_CTYPE   3

  rc = dbsql_declare_vtab(db,
     "CREATE TABLE x(value,pointer hidden,count hidden,ctype hidden)");
  if( rc==DBSQL_OK ){
    p_new = *vtab = dbsql_malloc( sizeof(*p_new) );
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
  }
  return rc;
}

/*
** This method is the destructor for carray_cursor objects.
*/
static int __carray_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Constructor for a new carray_cursor object.
*/
static int __carray_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  carray_cursor *cur;
  cur = dbsql_malloc( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Destructor for a carray_cursor.
*/
static int __carray_close(dbsql_vtab_cursor_t *cur){
  dbsql_free(cur);
  return DBSQL_OK;
}


/*
** Advance a carray_cursor to its next row of output.
*/
static int __carray_next(dbsql_vtab_cursor_t *cur){
  carray_cursor *p_cur = (carray_cursor*)cur;
  p_cur->i_rowid++;
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the carray_cursor
** is currently pointing.
*/
static int __carray_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  carray_cursor *p_cur = (carray_cursor*)cur;
  int64_t x = 0;
  switch( i ){
    case CARRAY_COLUMN_POINTER:   return DBSQL_OK;
    case CARRAY_COLUMN_COUNT:     x = p_cur->i_cnt;   break;
    case CARRAY_COLUMN_CTYPE: {
      dbsql_result_text(ctx, __az_carray_type[p_cur->type], -1, DBSQL_STATIC);
      return DBSQL_OK;
    }
    default: {
      switch( p_cur->type ){
        case CARRAY_INT32: {
          int *p = (int*)p_cur->ptr;
          dbsql_result_int(ctx, p[p_cur->i_rowid-1]);
          return DBSQL_OK;
        }
        case CARRAY_INT64: {
          int64_t *p = (int64_t*)p_cur->ptr;
          dbsql_result_int64(ctx, p[p_cur->i_rowid-1]);
          return DBSQL_OK;
        }
        case CARRAY_DOUBLE: {
          double *p = (double*)p_cur->ptr;
          dbsql_result_double(ctx, p[p_cur->i_rowid-1]);
          return DBSQL_OK;
        }
        case CARRAY_TEXT: {
          const char **p = (const char**)p_cur->ptr;
          dbsql_result_text(ctx, p[p_cur->i_rowid-1], -1, DBSQL_TRANSIENT);
          return DBSQL_OK;
        }
        default: {
          const struct iovec *p = (struct iovec*)p_cur->ptr;
          assert( p_cur->type==CARRAY_BLOB );
          dbsql_result_blob(ctx, p[p_cur->i_rowid-1].iov_base,
                              (int)p[p_cur->i_rowid-1].iov_len, DBSQL_TRANSIENT);
          return DBSQL_OK;
        }
      }
    }
  }
  dbsql_result_int64(ctx, x);
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int __carray_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  carray_cursor *p_cur = (carray_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __carray_eof(dbsql_vtab_cursor_t *cur){
  carray_cursor *p_cur = (carray_cursor*)cur;
  return p_cur->i_rowid>p_cur->i_cnt;
}

/*
** This method is called to "rewind" the carray_cursor object back
** to the first row of output.
*/
static int __carray_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  carray_cursor *cur = (carray_cursor *)vtab_cursor;
  cur->ptr = 0;
  cur->i_cnt = 0;
  switch( idx_num ){
    case 1: {
      carray_bind *bind = dbsql_value_pointer(argv[0], "carray-bind");
      if( bind==0 ) break;
      cur->ptr = bind->data;
      cur->i_cnt = bind->n_data;
      cur->type = bind->m_flags & 0x07;
      break;
    }
    case 2:
    case 3: {
      cur->ptr = dbsql_value_pointer(argv[0], "carray");
      cur->i_cnt = cur->ptr ? dbsql_value_int64(argv[1]) : 0;
      if( idx_num<3 ){
        cur->type = CARRAY_INT32;
      }else{
        unsigned char i;
        const char *type = (const char*)dbsql_value_text(argv[2]);
        for(i=0; i<sizeof(__az_carray_type)/sizeof(__az_carray_type[0]); i++){
          if( dbsql_stricmp(type, __az_carray_type[i])==0 ) break;
        }
        if( i>=sizeof(__az_carray_type)/sizeof(__az_carray_type[0]) ){
          vtab_cursor->vtab->errmsg = dbsql_mprintf(
            "unknown datatype: %Q", type);
          return DBSQL_ERROR;
        }else{
          cur->type = i;
        }
      }
      break;
    }
  }
  cur->i_rowid = 1;
  return DBSQL_OK;
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the carray virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
**
** In this implementation idx_num is used to represent the
** query plan.  idx_str is unused.
**
** idx_num is:
**
**    1    If only the pointer= constraint exists.  In this case, the
**         parameter must be bound using dbsql_carray_bind().
**
**    2    if the pointer= and count= constraints exist.
**
**    3    if the ctype= constraint also exists.
**
** idx_num is 0 otherwise and carray becomes an empty table.
*/
static int __carray_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;                 /* Loop over constraints */
  int ptr_idx = -1;       /* index_t of the pointer= constraint, or -1 if none */
  int cnt_idx = -1;       /* index_t of the count= constraint, or -1 if none */
  int ctype_idx = -1;     /* index_t of the ctype= constraint, or -1 if none */
  unsigned seen = 0;     /* Bitmask of == constrainted columns */

  const struct dbsql_index_constraint_t *constraint;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->op!=DBSQL_INDEX_CONSTRAINT_EQ ) continue;
    if( constraint->i_column>=0 ) seen |= 1 << constraint->i_column;
    if( constraint->usable==0 ) continue;
    switch( constraint->i_column ){
      case CARRAY_COLUMN_POINTER:
        ptr_idx = i;
        break;
      case CARRAY_COLUMN_COUNT:
        cnt_idx = i;
        break;
      case CARRAY_COLUMN_CTYPE:
        ctype_idx = i;
        break;
    }
  }
  if( ptr_idx>=0 ){
    idx_info->constraint_usages[ptr_idx].argv_index = 1;
    idx_info->constraint_usages[ptr_idx].omit = 1;
    idx_info->estimated_cost = (double)1;
    idx_info->estimated_rows = 100;
    idx_info->idx_num = 1;
    if( cnt_idx>=0 ){
      idx_info->constraint_usages[cnt_idx].argv_index = 2;
      idx_info->constraint_usages[cnt_idx].omit = 1;
      idx_info->idx_num = 2;
      if( ctype_idx>=0 ){
        idx_info->constraint_usages[ctype_idx].argv_index = 3;
        idx_info->constraint_usages[ctype_idx].omit = 1;
        idx_info->idx_num = 3;
      }else if( seen & (1<<CARRAY_COLUMN_CTYPE) ){
        /* In a three-argument carray(), we need to know the value of all
        ** three arguments */
        return DBSQL_CONSTRAINT;
      }
    }else if( seen & (1<<CARRAY_COLUMN_COUNT) ){
      /* In a two-argument carray(), we need to know the value of both
      ** arguments */
      return DBSQL_CONSTRAINT;
    }
  }else{
    idx_info->estimated_cost = (double)2147483647;
    idx_info->estimated_rows = 2147483647;
    idx_info->idx_num = 0;
  }
  return DBSQL_OK;
}

/*
** This following structure defines all the methods for the 
** carray virtual table.
*/
static dbsql_module_t __carray_module = {
  0,                         /* i_version */
  0,                         /* create */
  __carray_connect,             /* connect */
  __carray_best_index,           /* best_index */
  __carray_disconnect,          /* disconnect */
  0,                         /* destroy */
  __carray_open,                /* x_open - open a cursor */
  __carray_close,               /* x_close - close a cursor */
  __carray_filter,              /* filter - configure scan constraints */
  __carray_next,                /* next - advance a cursor */
  __carray_eof,                 /* eof - check for end of scan */
  __carray_column,              /* column - read data */
  __carray_rowid,               /* rowid - read data */
  0,                         /* update */
  0,                         /* begin */
  0,                         /* x_sync */
  0,                         /* commit */
  0,                         /* rollback */
  0,                         /* find_method */
  0,                         /* rename */
  0,                         /* savepoint */
  0,                         /* release */
  0,                         /* rollback_to */
  0,                         /* shadow */
  0                          /* integrity */
};

/*
** Destructor for the carray_bind object
*/
static void __carray_bind_del(void *ptr){
  carray_bind *p = (carray_bind*)ptr;
  if( p->x_del!=DBSQL_STATIC ){
    p->x_del(p->del);
  }
  dbsql_free(p);
}

/*
** Invoke this interface in order to bind to the single-argument
** version of CARRAY().
**
**    stmt        The prepared statement to which to bind
**    idx          The index of the parameter of stmt to which to bind
**    data        The data to be bound
**    n_data        The number of elements in data
**    m_flags       One of DBSQL_CARRAY_xxxx indicating datatype of data
**    destroy     Destructor for destroy or data if destroy==NULL.
**    destroy     Invoke destroy on this pointer if not NULL
**
** The destructor is called destroy if destroy!=NULL, or against
** data if destroy==NULL.
*/
DBSQL_API int dbsql_carray_bind_v2(
  dbsql_stmt_t *stmt,
  int idx,
  void *data,
  int n_data,
  int m_flags,
  void (*x_destroy)(void*),
  void *destroy
){
  carray_bind *p_new = 0;
  int i;
  int rc = DBSQL_OK;
  
  /* Ensure that the m_flags value is acceptable. */
  assert( CARRAY_INT32==0 && CARRAY_INT64==1 && CARRAY_DOUBLE==2 );
  assert( CARRAY_TEXT==3 && CARRAY_BLOB==4 );
  if( m_flags<CARRAY_INT32 || m_flags>CARRAY_BLOB ){
    rc = DBSQL_ERROR;
    goto carray_bind_error;
  }

  p_new = dbsql_malloc64(sizeof(*p_new));
  if( p_new==0 ){
    rc = DBSQL_NOMEM;
    goto carray_bind_error;
  }

  p_new->n_data = n_data;
  p_new->m_flags = m_flags;
  if( x_destroy==DBSQL_TRANSIENT ){
    int64_t sz = n_data;
    switch( m_flags ){
      case CARRAY_INT32:   sz *= 4;                     break;
      case CARRAY_INT64:   sz *= 8;                     break;
      case CARRAY_DOUBLE:  sz *= 8;                     break;
      case CARRAY_TEXT:    sz *= sizeof(char*);         break;
      default:             sz *= sizeof(struct iovec);  break;
    }
    if( m_flags==CARRAY_TEXT ){
      for(i=0; i<n_data; i++){
        const char *z = ((char**)data)[i];
        if( z ) sz += strlen(z) + 1;
      }
    }else if( m_flags==CARRAY_BLOB ){
      for(i=0; i<n_data; i++){
        sz += ((struct iovec*)data)[i].iov_len;
      }
    } 

    p_new->data = dbsql_malloc64( sz );
    if( p_new->data==0 ){
      rc = DBSQL_NOMEM;
      goto carray_bind_error;
    }

    if( m_flags==CARRAY_TEXT ){
      char **az = (char**)p_new->data;
      char *z = (char*)&az[n_data];
      for(i=0; i<n_data; i++){
        const char *z_data = ((char**)data)[i];
        int64_t n;
        if( z_data==0 ){
          az[i] = 0;
          continue;
        }
        az[i] = z;
        n = strlen(z_data);
        memcpy(z, z_data, n+1);
        z += n+1;
      }
    }else if( m_flags==CARRAY_BLOB ){
      struct iovec *p = (struct iovec*)p_new->data;
      unsigned char *z = (unsigned char*)&p[n_data];
      for(i=0; i<n_data; i++){
        size_t n = ((struct iovec*)data)[i].iov_len;
        p[i].iov_len = n;
        p[i].iov_base = z;
        z += n;
        memcpy(p[i].iov_base, ((struct iovec*)data)[i].iov_base, n);
      }
    }else{
      memcpy(p_new->data, data, sz);
    }
    p_new->x_del = dbsql_free;
    p_new->del = p_new->data;
  }else{
    p_new->data = data;
    p_new->x_del = x_destroy;
    p_new->del = destroy;
  }
  return dbsql_bind_pointer(stmt, idx, p_new, "carray-bind", __carray_bind_del);
 
 carray_bind_error:
  if( x_destroy!=DBSQL_STATIC && x_destroy!=DBSQL_TRANSIENT ){
    x_destroy(destroy);
  }
  dbsql_free(p_new);
  return rc;
}

/*
** Invoke this interface in order to bind to the single-argument
** version of CARRAY().  Same as dbsql_carray_bind_v2() with the
** destroy parameter set to NULL.
*/
DBSQL_API int dbsql_carray_bind(
  dbsql_stmt_t *stmt,
  int idx,
  void *data,
  int n_data,
  int m_flags,
  void (*destroy)(void*)
){
  return dbsql_carray_bind_v2(stmt,idx,data,n_data,m_flags,destroy,data);
}

/*
** Invoke this routine to register the carray() function.
*/
module_def_t *__carray_register(DBSQL *db){
  return __vtab_create_module(db, "carray", &__carray_module, 0, 0);
}

#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) && defined(DBSQL_ENABLE_CARRAY) */
