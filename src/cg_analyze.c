/*
** 2005-07-08
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code associated with the ANALYZE command.
**
** The ANALYZE command gather statistics about the content of tables
** and indices.  These statistics are made available to the query planner
** to help it make better decisions about how to perform queries.
**
** The following system tables are or have been supported:
**
**    CREATE TABLE dbsql_stat1(tbl, idx, stat);
**    CREATE TABLE dbsql_stat2(tbl, idx, sampleno, sample);
**    CREATE TABLE dbsql_stat3(tbl, idx, n_eq, n_lt, n_d_lt, sample);
**    CREATE TABLE dbsql_stat4(tbl, idx, n_eq, n_lt, n_d_lt, sample);
**
** Additional tables might be added in future releases of dbsql.
** The dbsql_stat2 table is not created or used unless the dbsql version
** is between 3.6.18 and 3.7.8, inclusive, and unless dbsql is compiled
** with DBSQL_ENABLE_STAT2.  The dbsql_stat2 table is deprecated.
** The dbsql_stat2 table is superseded by dbsql_stat3, which is only
** created and used by dbsql versions 3.7.9 through 3.29.0 when
** DBSQL_ENABLE_STAT3 defined.  The functionality of dbsql_stat3
** is a superset of dbsql_stat2 and is also now deprecated.  The
** dbsql_stat4 is an enhanced version of dbsql_stat3 and is only 
** available when compiled with DBSQL_ENABLE_STAT4 and in dbsql
** versions 3.8.1 and later.  STAT4 is the only variant that is still
** supported.
**
** For most applications, dbsql_stat1 provides all the statistics required
** for the query planner to make good choices.
**
** Format of dbsql_stat1:
**
** There is normally one row per index, with the index identified by the
** name in the idx column.  The tbl column is the name of the table to
** which the index belongs.  In each such row, the stat column will be
** a string consisting of a list of integers.  The first integer in this
** list is the number of rows in the index.  (This is the same as the
** number of rows in the table, except for partial indices.)  The second
** integer is the average number of rows in the index that have the same
** value in the first column of the index.  The third integer is the average
** number of rows in the index that have the same value for the first two
** columns.  The N-th integer (for N>1) is the average number of rows in 
** the index which have the same value for the first N-1 columns.  For
** a K-column index, there will be K+1 integers in the stat column.  If
** the index is unique, then the last integer will be 1.
**
** The list of integers in the stat column can optionally be followed
** by the keyword "unordered".  The "unordered" keyword, if it is present,
** must be separated from the last integer by a single space.  If the
** "unordered" keyword is present, then the query planner assumes that
** the index is unordered and will not use the index for a range query.
** 
** If the dbsql_stat1.idx column is NULL, then the dbsql_stat1.stat
** column contains a single integer which is the (estimated) number of
** rows in the table identified by dbsql_stat1.tbl.
**
** Format of dbsql_stat2:
**
** The dbsql_stat2 is only created and is only used if dbsql is compiled
** with DBSQL_ENABLE_STAT2 and if the dbsql version number is between
** 3.6.18 and 3.7.8.  The "stat2" table contains additional information
** about the distribution of keys within an index.  The index is identified by
** the "idx" column and the "tbl" column is the name of the table to which
** the index belongs.  There are usually 10 rows in the dbsql_stat2
** table for each index.
**
** The dbsql_stat2 entries for an index that have sampleno between 0 and 9
** inclusive are samples of the left-most key value in the index taken at
** evenly spaced points along the index.  Let the number of samples be S
** (10 in the standard build) and let C be the number of rows in the index.
** Then the sampled rows are given by:
**
**     rownumber = (i*C*2 + C)/(S*2)
**
** For i between 0 and S-1.  Conceptually, the index space is divided into
** S uniform buckets and the samples are the middle row from each bucket.
**
** The format for dbsql_stat2 is recorded here for legacy reference.  This
** version of dbsql does not support dbsql_stat2.  It neither reads nor
** writes the dbsql_stat2 table.  This version of dbsql only supports
** dbsql_stat3.
**
** Format for dbsql_stat3:
**
** The dbsql_stat3 format is a subset of dbsql_stat4.  Hence, the
** dbsql_stat4 format will be described first.  Further information
** about dbsql_stat3 follows the dbsql_stat4 description.
**
** Format for dbsql_stat4:
**
** As with dbsql_stat2, the dbsql_stat4 table contains histogram data
** to aid the query planner in choosing good indices based on the values
** that indexed columns are compared against in the WHERE clauses of
** queries.
**
** The dbsql_stat4 table contains multiple entries for each index.
** The idx column names the index and the tbl column is the table of the
** index.  If the idx and tbl columns are the same, then the sample is
** of the INTEGER PRIMARY KEY.  The sample column is a blob which is the
** binary encoding of a key from the index.  The n_eq column is a
** list of integers.  The first integer is the approximate number
** of entries in the index whose left-most column exactly matches
** the left-most column of the sample.  The second integer in n_eq
** is the approximate number of entries in the index where the
** first two columns match the first two columns of the sample.
** And so forth.  n_lt is another list of integers that show the approximate
** number of entries that are strictly less than the sample.  The first
** integer in n_lt contains the number of entries in the index where the
** left-most column is less than the left-most column of the sample.
** The K-th integer in the n_lt entry is the number of index entries 
** where the first K columns are less than the first K columns of the
** sample.  The n_d_lt column is like n_lt except that it contains the 
** number of distinct entries in the index that are less than the
** sample.
**
** There can be an arbitrary number of dbsql_stat4 entries per index.
** The ANALYZE command will typically generate dbsql_stat4 tables
** that contain between 10 and 40 samples which are distributed across
** the key space, though not uniformly, and which include samples with
** large n_eq values.
**
** Format for dbsql_stat3 redux:
**
** The dbsql_stat3 table is like dbsql_stat4 except that it only
** looks at the left-most column of the index.  The dbsql_stat3.sample
** column contains the actual value of the left-most column instead
** of a blob encoding of the complete index key as is found in
** dbsql_stat4.sample.  The n_eq, n_lt, and n_d_lt entries of dbsql_stat3
** all contain just a single integer which is the same as the first
** integer in the equivalent columns in dbsql_stat4.
*/
#ifndef DBSQL_OMIT_ANALYZE
#include "dbsql_int.h"

#if defined(DBSQL_ENABLE_STAT4)
# define IS_STAT4     1
#else
# define IS_STAT4     0
# undef DBSQL_STAT4_SAMPLES
# define DBSQL_STAT4_SAMPLES 1
#endif

/*
** This routine generates code that opens the dbsql_statN tables.
** The dbsql_stat1 table is always relevant.  dbsql_stat2 is now
** obsolete.  dbsql_stat3 and dbsql_stat4 are only opened when
** appropriate compile-time options are provided.
**
** If the dbsql_statN tables do not previously exist, it is created.
**
** Argument where may be a pointer to a buffer containing a table name,
** or it may be a NULL pointer. If it is not NULL, then all entries in
** the dbsql_statN tables associated with the named table are deleted.
** If where==0, then code is generated to delete all stat table entries.
*/
static void __open_stat_table(
  parser_t *parser,          /* Parsing context */
  int i_db,                /* The database we are looking in */
  int i_stat_cur,           /* Open the dbsql_stat1 table on this cursor */
  const char *where,     /* Delete entries for this table or index */
  const char *where_type  /* Either "tbl" or "idx" */
){
  static const struct {
    const char *name;
    const char *cols;
  } tables[] = {
    { "dbsql_stat1", "tbl,idx,stat" },
#if defined(DBSQL_ENABLE_STAT4)
    { "dbsql_stat4", "tbl,idx,neq,nlt,ndlt,sample" },
#else
    { "dbsql_stat4", 0 },
#endif
    { "dbsql_stat3", 0 },
  };
  int i;
  DBSQL *db = parser->db;
  db_t *p_db;
  vdbe_t *v = __get_vdbe(parser);
  u_int32_t roots[ARRAY_SIZE(tables)];
  u_int8_t create_tbls[ARRAY_SIZE(tables)];
#ifdef DBSQL_ENABLE_STAT4
  const int n_to_open = OPTIMIZATION_ENABLED(db,DBSQL_Stat4) ? 2 : 1;
#else
  const int n_to_open = 1;
#endif

  if( v==0 ) return;
  assert( __sm_holds_all_mutexes(db) );
  assert( __vdbe_db(v)==db );
  p_db = &db->dbs[i_db];

  /* Create new statistic tables if they do not exist, or clear them
  ** if they do already exist.
  */
  for(i=0; i<ARRAY_SIZE(tables); i++){
    const char *tab = tables[i].name;
    table_t *p_stat;
    create_tbls[i] = 0;
    if( (p_stat = __find_table(db, tab, p_db->db_s_name))==0 ){
      if( i<n_to_open ){
        /* The dbsql_statN table does not exist. Create it. Note that a 
        ** side-effect of the CREATE TABLE statement is to leave the rootpage 
        ** of the new table in register parser->reg_root. This is important 
        ** because the __open_write opcode below will be needing it. */
        __nested_parse(parser,
            "CREATE TABLE %Q.%s(%s)", p_db->db_s_name, tab, tables[i].cols
        );
        assert( parser->is_create || parser->n_err );
        roots[i] = (u_int32_t)parser->u1.cr.reg_root;
        create_tbls[i] = OPFLAG_P2ISREG;
      }
    }else{
      /* The table already exists. If where is not NULL, delete all entries 
      ** associated with the table where. If where is NULL, delete the
      ** entire contents of the table. */
      roots[i] = p_stat->tnum;
      __table_lock(parser, i_db, roots[i], 1, tab);
      if( where ){
        __nested_parse(parser,
           "DELETE FROM %Q.%s WHERE %s=%Q",
           p_db->db_s_name, tab, where_type, where
        );
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
      }else if( db->pre_update_callback ){
        __nested_parse(parser, "DELETE FROM %Q.%s", p_db->db_s_name, tab);
#endif
      }else{
        /* The dbsql_stat[134] table already exists.  Delete all rows. */
        __vdbe_add_op2(v, OP_Clear, (int)roots[i], i_db);
      }
    }
  }

  /* Open the dbsql_stat[134] tables for writing. */
  for(i=0; i<n_to_open; i++){
    assert( i<ARRAY_SIZE(tables) );
    __vdbe_add_op4_int(v, OP_OpenWrite, i_stat_cur+i, (int)roots[i], i_db, 3);
    __vdbe_change_p5(v, create_tbls[i]);
    VDBE_COMMENT((v, tables[i].name));
  }
}

/*
** Recommended number of samples for dbsql_stat4
*/
#ifndef DBSQL_STAT4_SAMPLES
# define DBSQL_STAT4_SAMPLES 24
#endif

/*
** Three SQL functions - stat_init(), stat_push(), and stat_get() -
** share an instance of the following structure to hold their state
** information.
*/
typedef struct stat_accum_t stat_accum_t;
typedef struct stat_sample_t stat_sample_t;
struct stat_sample_t {
  rowcnt_t *an_d_lt;                 /* dbsql_stat4.n_d_lt */
#ifdef DBSQL_ENABLE_STAT4
  rowcnt_t *an_eq;                  /* dbsql_stat4.n_eq */
  rowcnt_t *an_lt;                  /* dbsql_stat4.n_lt */
  union {
    int64_t i_rowid;                     /* Rowid in main table of the key */
    u_int8_t *rowids;                     /* Key for WITHOUT ROWID tables */
  } u;
  u_int32_t n_rowid;                     /* Sizeof rowids[] */
  u_int8_t is_p_sample;                   /* True if a periodic sample */
  int i_col;                       /* If !is_p_sample, the reason for inclusion */
  u_int32_t i_hash;                      /* Tiebreaker hash */
#endif
};                                                    
struct stat_accum_t {
  DBSQL *db;              /* Database connection, for malloc() */
  rowcnt_t n_est;             /* Estimated number of rows */
  rowcnt_t n_row;             /* Number of rows visited so far */
  int n_limit;               /* Analysis row-scan limit */
  int n_col;                 /* Number of columns in index + pk/rowid */
  int n_key_col;              /* Number of index columns w/o the pk/rowid */
  u_int8_t n_skip_ahead;            /* Number of times of skip-ahead */
  stat_sample_t current;       /* Current row as a stat_sample_t */
#ifdef DBSQL_ENABLE_STAT4
  rowcnt_t n_p_sample;         /* How often to do a periodic sample */
  int mx_sample;             /* Maximum number of samples to accumulate */
  u_int32_t i_prn;                 /* Pseudo-random number used for sampling */
  stat_sample_t *bests;        /* Array of n_col best samples */
  int i_min;                 /* index_t in a[] of entry with minimum score */
  int n_sample;              /* Current number of samples */
  int n_max_eq_zero;           /* Max leading 0 in an_eq[] for any a[] entry */
  int i_get;                 /* index_t of current sample accessed by stat_get() */
  stat_sample_t *a;            /* Array of mx_sample stat_sample_t objects */
#endif
};

/* Reclaim memory used by a stat_sample_t
*/
#ifdef DBSQL_ENABLE_STAT4
static void __sample_clear(DBSQL *db, stat_sample_t *p){
  assert( db!=0 );
  if( p->n_rowid ){
    __dbsql_db_free(db, p->u.rowids);
    p->n_rowid = 0;
  }
}
#endif

/* Initialize the BLOB value of a ROWID
*/
#ifdef DBSQL_ENABLE_STAT4
static void __sample_set_rowid(DBSQL *db, stat_sample_t *p, int n, const u_int8_t *data){
  assert( db!=0 );
  if( p->n_rowid ) __dbsql_db_free(db, p->u.rowids);
  p->u.rowids = __db_malloc_raw_nn(db, n);
  if( p->u.rowids ){
    p->n_rowid = n;
    memcpy(p->u.rowids, data, n);
  }else{
    p->n_rowid = 0;
  }
}
#endif

/* Initialize the INTEGER value of a ROWID.
*/
#ifdef DBSQL_ENABLE_STAT4
static void __sample_set_rowid_int64(DBSQL *db, stat_sample_t *p, int64_t i_rowid){
  assert( db!=0 );
  if( p->n_rowid ) __dbsql_db_free(db, p->u.rowids);
  p->n_rowid = 0;
  p->u.i_rowid = i_rowid;
}
#endif


/*
** Copy the contents of object (*from) into (*to).
*/
#ifdef DBSQL_ENABLE_STAT4
static void __sample_copy(stat_accum_t *p, stat_sample_t *to, stat_sample_t *from){
  to->is_p_sample = from->is_p_sample;
  to->i_col = from->i_col;
  to->i_hash = from->i_hash;
  memcpy(to->an_eq, from->an_eq, sizeof(rowcnt_t)*p->n_col);
  memcpy(to->an_lt, from->an_lt, sizeof(rowcnt_t)*p->n_col);
  memcpy(to->an_d_lt, from->an_d_lt, sizeof(rowcnt_t)*p->n_col);
  if( from->n_rowid ){
    __sample_set_rowid(p->db, to, from->n_rowid, from->u.rowids);
  }else{
    __sample_set_rowid_int64(p->db, to, from->u.i_rowid);
  }
}
#endif

/*
** Reclaim all memory of a stat_accum_t structure.
*/
static void __stat_accum_destructor(void *old){
  stat_accum_t *p = (stat_accum_t*)old;
#ifdef DBSQL_ENABLE_STAT4
  if( p->mx_sample ){
    int i;
    for(i=0; i<p->n_col; i++) __sample_clear(p->db, p->bests+i);
    for(i=0; i<p->mx_sample; i++) __sample_clear(p->db, p->a+i);
    __sample_clear(p->db, &p->current);
  }
#endif
  __dbsql_db_free(p->db, p);
}

/*
** Implementation of the stat_init(N,K,C,L) SQL function. The four parameters
** are:
**     N:    The number of columns in the index including the rowid/pk (note 1)
**     K:    The number of columns in the index excluding the rowid/pk.
**     C:    Estimated number of rows in the index
**     L:    A limit on the number of rows to scan, or 0 for no-limit 
**
** Note 1:  In the special case of the covering index that implements a
** WITHOUT ROWID table, N is the number of PRIMARY KEY columns, not the
** total number of columns in the table.
**
** For indexes on ordinary rowid tables, N==K+1.  But for indexes on
** WITHOUT ROWID tables, N=K+P where P is the number of columns in the
** PRIMARY KEY of the table.  The covering index that implements the
** original WITHOUT ROWID table as N==K as a special case.
**
** This routine allocates the stat_accum_t object in heap memory. The return 
** value is a pointer to the stat_accum_t object.  The datatype of the
** return value is BLOB, but it is really just a pointer to the stat_accum_t
** object.
*/
static void __stat_init(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  stat_accum_t *p;
  int n_col;                       /* Number of columns in index being sampled */
  int n_key_col;                    /* Number of key columns */
  int n_col_up;                     /* n_col rounded up for alignment */
  int64_t n;                          /* Bytes of space to allocate */
  DBSQL *db = dbsql_context_db_handle(context);   /* Database connection */
#ifdef DBSQL_ENABLE_STAT4
  /* Maximum number of samples.  0 if STAT4 data is not collected */
  int mx_sample = OPTIMIZATION_ENABLED(db,DBSQL_Stat4) ?DBSQL_STAT4_SAMPLES :0;
#endif

  /* Decode the three function arguments */
  UNUSED_PARAMETER(argc);
  n_col = dbsql_value_int(argv[0]);
  assert( n_col>0 );
  n_col_up = sizeof(rowcnt_t)<8 ? (n_col+1)&~1 : n_col;
  n_key_col = dbsql_value_int(argv[1]);
  assert( n_key_col<=n_col );
  assert( n_key_col>0 );

  /* Allocate the space required for the stat_accum_t object */
  n = sizeof(*p) 
    + sizeof(rowcnt_t)*n_col_up;                    /* stat_accum_t.an_d_lt */
#ifdef DBSQL_ENABLE_STAT4
  n += sizeof(rowcnt_t)*n_col_up;                   /* stat_accum_t.an_eq */
  if( mx_sample ){
    n += sizeof(rowcnt_t)*n_col_up                  /* stat_accum_t.an_lt */
      + sizeof(stat_sample_t)*(n_col+mx_sample)       /* stat_accum_t.bests[], a[] */
      + sizeof(rowcnt_t)*3*n_col_up*(n_col+mx_sample);
  }
#endif
  p = __db_malloc_zero(db, n);
  if( p==0 ){
    dbsql_result_error_nomem(context);
    return;
  }

  p->db = db;
  p->n_est = dbsql_value_int64(argv[2]);
  p->n_row = 0;
  p->n_limit = dbsql_value_int(argv[3]);
  p->n_col = n_col;
  p->n_key_col = n_key_col;
  p->n_skip_ahead = 0;
  p->current.an_d_lt = (rowcnt_t*)&p[1];

#ifdef DBSQL_ENABLE_STAT4
  p->current.an_eq = &p->current.an_d_lt[n_col_up];
  p->mx_sample = p->n_limit==0 ? mx_sample : 0;
  if( mx_sample ){
    u_int8_t *space;                     /* Allocated space not yet assigned */
    int i;                          /* Used to iterate through p->samples[] */

    p->i_get = -1;
    p->n_p_sample = (rowcnt_t)(p->n_est/(mx_sample/3+1) + 1);
    p->current.an_lt = &p->current.an_eq[n_col_up];
    p->i_prn = 0x689e962d*(u_int32_t)n_col ^ 0xd0944565*(u_int32_t)dbsql_value_int(argv[2]);
  
    /* Set up the stat_accum_t.a[] and bests[] arrays */
    p->a = (struct stat_sample_t*)&p->current.an_lt[n_col_up];
    p->bests = &p->a[mx_sample];
    space = (u_int8_t*)(&p->a[mx_sample+n_col]);
    for(i=0; i<(mx_sample+n_col); i++){
      p->a[i].an_eq = (rowcnt_t *)space; space += (sizeof(rowcnt_t) * n_col_up);
      p->a[i].an_lt = (rowcnt_t *)space; space += (sizeof(rowcnt_t) * n_col_up);
      p->a[i].an_d_lt = (rowcnt_t *)space; space += (sizeof(rowcnt_t) * n_col_up);
    }
    assert( (space - (u_int8_t*)p)==n );
  
    for(i=0; i<n_col; i++){
      p->bests[i].i_col = i;
    }
  }
#endif

  /* Return a pointer to the allocated object to the caller.  Note that
  ** only the pointer (the 2nd parameter) matters.  The size of the object
  ** (given by the 3rd parameter) is never used and can be any positive
  ** value. */
  dbsql_result_blob(context, p, sizeof(*p), __stat_accum_destructor);
}
static const func_def_t __stat_init_funcdef = {
  4,               /* n_arg */
  DBSQL_UTF8,     /* func_flags */
  0,               /* user_data */
  0,               /* next */
  __stat_init,        /* s_func */
  0,               /* finalize */
  0, 0,            /* value, inverse */
  "stat_init",     /* name */
  {0}
};

#ifdef DBSQL_ENABLE_STAT4
/*
** p_new and old are both candidate non-periodic samples selected for 
** the same column (p_new->i_col==old->i_col). Ignoring this column and 
** considering only any trailing columns and the sample hash value, this
** function returns true if sample p_new is to be preferred over old.
** In other words, if we assume that the cardinalities of the selected
** column for p_new and old are equal, is p_new to be preferred over old.
**
** This function assumes that for each argument sample, the contents of
** the an_eq[] array from sample->an_eq[sample->i_col+1] onwards are valid. 
*/
static int __sample_is_better_post(
  stat_accum_t *accum, 
  stat_sample_t *p_new, 
  stat_sample_t *old
){
  int n_col = accum->n_col;
  int i;
  assert( p_new->i_col==old->i_col );
  for(i=p_new->i_col+1; i<n_col; i++){
    if( p_new->an_eq[i]>old->an_eq[i] ) return 1;
    if( p_new->an_eq[i]<old->an_eq[i] ) return 0;
  }
  if( p_new->i_hash>old->i_hash ) return 1;
  return 0;
}
#endif

#ifdef DBSQL_ENABLE_STAT4
/*
** Return true if p_new is to be preferred over old.
**
** This function assumes that for each argument sample, the contents of
** the an_eq[] array from sample->an_eq[sample->i_col] onwards are valid. 
*/
static int __sample_is_better(
  stat_accum_t *accum, 
  stat_sample_t *p_new, 
  stat_sample_t *old
){
  rowcnt_t n_eq_new = p_new->an_eq[p_new->i_col];
  rowcnt_t n_eq_old = old->an_eq[old->i_col];

  assert( old->is_p_sample==0 && p_new->is_p_sample==0 );
  assert( IS_STAT4 || (p_new->i_col==0 && old->i_col==0) );

  if( (n_eq_new>n_eq_old) ) return 1;
  if( n_eq_new==n_eq_old ){
    if( p_new->i_col<old->i_col ) return 1;
    return (p_new->i_col==old->i_col && __sample_is_better_post(accum, p_new, old));
  }
  return 0;
}

/*
** Copy the contents of sample *p_new into the p->a[] array. If necessary,
** remove the least desirable sample from p->a[] to make room.
*/
static void __sample_insert(stat_accum_t *p, stat_sample_t *p_new, int n_eq_zero){
  stat_sample_t *sample = 0;
  int i;

  assert( IS_STAT4 || n_eq_zero==0 );

  /* stat_accum_t.n_max_eq_zero is set to the maximum number of leading 0
  ** values in the an_eq[] array of any sample in stat_accum_t.a[]. In
  ** other words, if n_max_eq_zero is n, then it is guaranteed that there
  ** are no samples with stat_sample_t.an_eq[m]==0 for (m>=n). */
  if( n_eq_zero>p->n_max_eq_zero ){
    p->n_max_eq_zero = n_eq_zero;
  }
  if( p_new->is_p_sample==0 ){
    stat_sample_t *upgrade = 0;
    assert( p_new->an_eq[p_new->i_col]>0 );

    /* This sample is being added because the prefix that ends in column 
    ** i_col occurs many times in the table. However, if we have already
    ** added a sample that shares this prefix, there is no need to add
    ** this one. Instead, upgrade the priority of the highest priority
    ** existing sample that shares this prefix.  */
    for(i=p->n_sample-1; i>=0; i--){
      stat_sample_t *old = &p->a[i];
      if( old->an_eq[p_new->i_col]==0 ){
        if( old->is_p_sample ) return;
        assert( old->i_col>p_new->i_col );
        assert( __sample_is_better(p, p_new, old) );
        if( upgrade==0 || __sample_is_better(p, old, upgrade) ){
          upgrade = old;
        }
      }
    }
    if( upgrade ){
      upgrade->i_col = p_new->i_col;
      upgrade->an_eq[upgrade->i_col] = p_new->an_eq[upgrade->i_col];
      goto find_new_min;
    }
  }

  /* If necessary, remove sample i_min to make room for the new sample. */
  if( p->n_sample>=p->mx_sample ){
    stat_sample_t *min = &p->a[p->i_min];
    rowcnt_t *an_eq = min->an_eq;
    rowcnt_t *an_lt = min->an_lt;
    rowcnt_t *an_d_lt = min->an_d_lt;
    __sample_clear(p->db, min);
    memmove(min, &min[1], sizeof(p->a[0])*(p->n_sample-p->i_min-1));
    sample = &p->a[p->n_sample-1];
    sample->n_rowid = 0;
    sample->an_eq = an_eq;
    sample->an_d_lt = an_d_lt;
    sample->an_lt = an_lt;
    p->n_sample = p->mx_sample-1;
  }

  /* The "rows less-than" for the rowid column must be greater than that
  ** for the last sample in the p->a[] array. Otherwise, the samples would
  ** be out of order. */
  assert( p->n_sample==0 
       || p_new->an_lt[p->n_col-1] > p->a[p->n_sample-1].an_lt[p->n_col-1] );

  /* Insert the new sample */
  sample = &p->a[p->n_sample];
  __sample_copy(p, sample, p_new);
  p->n_sample++;

  /* Zero the first n_eq_zero entries in the an_eq[] array. */
  memset(sample->an_eq, 0, sizeof(rowcnt_t)*n_eq_zero);

find_new_min:
  if( p->n_sample>=p->mx_sample ){
    int i_min = -1;
    for(i=0; i<p->mx_sample; i++){
      if( p->a[i].is_p_sample ) continue;
      if( i_min<0 || __sample_is_better(p, &p->a[i_min], &p->a[i]) ){
        i_min = i;
      }
    }
    assert( i_min>=0 );
    p->i_min = i_min;
  }
}
#endif /* DBSQL_ENABLE_STAT4 */

#ifdef DBSQL_ENABLE_STAT4
/*
** Field i_chng of the index being scanned has changed. So at this point
** p->current contains a sample that reflects the previous row of the
** index. The value of an_eq[i_chng] and subsequent an_eq[] elements are
** correct at this point.
*/
static void __sample_push_previous(stat_accum_t *p, int i_chng){
  int i;

  /* Check if any samples from the bests[] array should be pushed
  ** into index_sample_t.a[] at this point.  */
  for(i=(p->n_col-2); i>=i_chng; i--){
    stat_sample_t *best = &p->bests[i];
    best->an_eq[i] = p->current.an_eq[i];
    if( p->n_sample<p->mx_sample || __sample_is_better(p, best, &p->a[p->i_min]) ){
      __sample_insert(p, best, i);
    }
  }

  /* Check that no sample contains an an_eq[] entry with an index of
  ** p->n_max_eq_zero or greater set to zero. */
  for(i=p->n_sample-1; i>=0; i--){
    int j;
    for(j=p->n_max_eq_zero; j<p->n_col; j++) assert( p->a[i].an_eq[j]>0 );
  }

  /* Update the an_eq[] fields of any samples already collected. */
  if( i_chng<p->n_max_eq_zero ){
    for(i=p->n_sample-1; i>=0; i--){
      int j;
      for(j=i_chng; j<p->n_col; j++){
        if( p->a[i].an_eq[j]==0 ) p->a[i].an_eq[j] = p->current.an_eq[j];
      }
    }
    p->n_max_eq_zero = i_chng;
  }
}
#endif /* DBSQL_ENABLE_STAT4 */

/*
** Implementation of the stat_push SQL function:  stat_push(P,C,R)
** Arguments:
**
**    P     Pointer to the stat_accum_t object created by stat_init()
**    C     index_t of left-most column to differ from previous row
**    R     Rowid for the current row.  Might be a key record for
**          WITHOUT ROWID tables.
**
** The purpose of this routine is to collect statistical data and/or
** samples from the index being analyzed into the stat_accum_t object.
** The stat_get() SQL function will be used afterwards to
** retrieve the information gathered.
**
** This SQL function usually returns NULL, but might return an integer
** if it wants the byte-code to do special processing.
**
** The R parameter is only used for STAT4
*/
static void __stat_push(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int i;

  /* The three function arguments */
  stat_accum_t *p = (stat_accum_t*)dbsql_value_blob(argv[0]);
  int i_chng = dbsql_value_int(argv[1]);

  UNUSED_PARAMETER( argc );
  UNUSED_PARAMETER( context );
  assert( p->n_col>0 );
  assert( i_chng<p->n_col );

  if( p->n_row==0 ){
    /* This is the first call to this function. Do initialization. */
#ifdef DBSQL_ENABLE_STAT4
    for(i=0; i<p->n_col; i++) p->current.an_eq[i] = 1;
#endif
  }else{
    /* Second and subsequent calls get processed here */
#ifdef DBSQL_ENABLE_STAT4
    if( p->mx_sample ) __sample_push_previous(p, i_chng);
#endif

    /* Update an_d_lt[], an_lt[] and an_eq[] to reflect the values that apply
    ** to the current row of the index. */
#ifdef DBSQL_ENABLE_STAT4
    for(i=0; i<i_chng; i++){
      p->current.an_eq[i]++;
    }
#endif
    for(i=i_chng; i<p->n_col; i++){
      p->current.an_d_lt[i]++;
#ifdef DBSQL_ENABLE_STAT4
      if( p->mx_sample ) p->current.an_lt[i] += p->current.an_eq[i];
      p->current.an_eq[i] = 1;
#endif
    }
  }

  p->n_row++;
#ifdef DBSQL_ENABLE_STAT4
  if( p->mx_sample ){
    rowcnt_t n_lt;
    if( dbsql_value_type(argv[2])==DBSQL_INTEGER ){
      __sample_set_rowid_int64(p->db, &p->current, dbsql_value_int64(argv[2]));
    }else{
      __sample_set_rowid(p->db, &p->current, dbsql_value_bytes(argv[2]),
                                         dbsql_value_blob(argv[2]));
    }
    p->current.i_hash = p->i_prn = p->i_prn*1103515245 + 12345;

    n_lt = p->current.an_lt[p->n_col-1];
    /* Check if this is to be a periodic sample. If so, add it. */
    if( (n_lt/p->n_p_sample)!=(n_lt+1)/p->n_p_sample ){
      p->current.is_p_sample = 1;
      p->current.i_col = 0;
      __sample_insert(p, &p->current, p->n_col-1);
      p->current.is_p_sample = 0;
    }

    /* Update the bests[] array. */
    for(i=0; i<(p->n_col-1); i++){
      p->current.i_col = i;
      if( i>=i_chng || __sample_is_better_post(p, &p->current, &p->bests[i]) ){
        __sample_copy(p, &p->bests[i], &p->current);
      }
    }
  }else
#endif
  if( p->n_limit && p->n_row>(rowcnt_t)p->n_limit*(p->n_skip_ahead+1) ){
    p->n_skip_ahead++;
    dbsql_result_int(context, p->current.an_d_lt[0]>0);
  }
}

static const func_def_t __stat_push_funcdef = {
  2+IS_STAT4,       /* n_arg */
  DBSQL_UTF8,     /* func_flags */
  0,               /* user_data */
  0,               /* next */
  __stat_push,        /* s_func */
  0,               /* finalize */
  0, 0,            /* value, inverse */
  "stat_push",     /* name */
  {0}
};

#define STAT_GET_STAT1 0          /* "stat" column of stat1 table */
#define STAT_GET_ROWID 1          /* "rowid" column of stat[34] entry */
#define STAT_GET_NEQ   2          /* "neq" column of stat[34] entry */
#define STAT_GET_NLT   3          /* "nlt" column of stat[34] entry */
#define STAT_GET_NDLT  4          /* "ndlt" column of stat[34] entry */

/*
** Implementation of the stat_get(P,J) SQL function.  This routine is
** used to query statistical information that has been gathered into
** the stat_accum_t object by prior calls to stat_push().  The P parameter
** has type BLOB but it is really just a pointer to the stat_accum_t object.
** The content to returned is determined by the parameter J
** which is one of the STAT_GET_xxxx values defined above.
**
** The stat_get(P,J) function is not available to generic SQL.  It is
** inserted as part of a manually constructed bytecode program.  (See
** the __call_stat_get() routine below.)  It is guaranteed that the P
** parameter will always be a pointer to a stat_accum_t object, never a
** NULL.
**
** If STAT4 is not enabled, then J is always
** STAT_GET_STAT1 and is hence omitted and this routine becomes
** a one-parameter function, stat_get(P), that always returns the
** stat1 table entry information.
*/
static void __stat_get(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  stat_accum_t *p = (stat_accum_t*)dbsql_value_blob(argv[0]);
#ifdef DBSQL_ENABLE_STAT4
  /* STAT4 has a parameter on this routine. */
  int call = dbsql_value_int(argv[1]);
  assert( argc==2 );
  assert( call==STAT_GET_STAT1 || call==STAT_GET_NEQ 
       || call==STAT_GET_ROWID || call==STAT_GET_NLT
       || call==STAT_GET_NDLT 
  );
  assert( call==STAT_GET_STAT1 || p->mx_sample );
  if( call==STAT_GET_STAT1 )
#else
  assert( argc==1 );
#endif
  {
    /* Return the value to store in the "stat" column of the dbsql_stat1
    ** table for this index.
    **
    ** The value is a string composed of a list of integers describing 
    ** the index. The first integer in the list is the total number of 
    ** entries in the index. There is one additional integer in the list 
    ** for each indexed column. This additional integer is an estimate of
    ** the number of rows matched by a equality query on the index using
    ** a key with the corresponding number of fields. In other words,
    ** if the index is on columns (a,b) and the dbsql_stat1 value is 
    ** "100 10 2", then dbsql estimates that:
    **
    **   * the index contains 100 rows,
    **   * "WHERE a=?" matches 10 rows, and
    **   * "WHERE a=? AND b=?" matches 2 rows.
    **
    ** If D is the count of distinct values and K is the total number of 
    ** rows, then each estimate is usually computed as:
    **
    **        I = (K+D-1)/D
    **
    ** In other words, I is K/D rounded up to the next whole integer.
    ** However, if I is between 1.0 and 1.1 (in other words if I is
    ** close to 1.0 but just a little larger) then do not round up but
    ** instead keep the I value at 1.0.
    */
    dbsql_str_t s_stat;   /* Text of the constructed "stat" line */
    int i;               /* Loop counter */

    __str_accum_init(&s_stat, 0, 0, 0, (p->n_key_col+1)*100);
    dbsql_str_appendf(&s_stat, "%llu", 
        p->n_skip_ahead ? (u_int64_t)p->n_est : (u_int64_t)p->n_row);
    for(i=0; i<p->n_key_col; i++){
      u_int64_t n_distinct = p->current.an_d_lt[i] + 1;
      u_int64_t i_val = (p->n_row + n_distinct - 1) / n_distinct;
      if( i_val==2 && p->n_row*10 <= n_distinct*11 ) i_val = 1;
      dbsql_str_appendf(&s_stat, " %llu", i_val);
#ifdef DBSQL_ENABLE_STAT4
      assert( p->current.an_eq[i] || p->n_row==0 );
#endif
    }
    __result_str_accum(context, &s_stat);
  }
#ifdef DBSQL_ENABLE_STAT4
  else if( call==STAT_GET_ROWID ){
    if( p->i_get<0 ){
      __sample_push_previous(p, 0);
      p->i_get = 0;
    }
    if( p->i_get<p->n_sample ){
      stat_sample_t *s = p->a + p->i_get;
      if( s->n_rowid==0 ){
        dbsql_result_int64(context, s->u.i_rowid);
      }else{
        dbsql_result_blob(context, s->u.rowids, s->n_rowid,
                            DBSQL_TRANSIENT);
      }
    }
  }else{
    rowcnt_t *cnts = 0;
    dbsql_str_t s_stat;
    int i;

    assert( p->i_get<p->n_sample );
    switch( call ){
      case STAT_GET_NEQ:  cnts = p->a[p->i_get].an_eq; break;
      case STAT_GET_NLT:  cnts = p->a[p->i_get].an_lt; break;
      default: {
        cnts = p->a[p->i_get].an_d_lt; 
        p->i_get++;
        break;
      }
    }
    __str_accum_init(&s_stat, 0, 0, 0, p->n_col*100);
    for(i=0; i<p->n_col; i++){
      dbsql_str_appendf(&s_stat, "%llu ", (u_int64_t)cnts[i]);
    }
    if( s_stat.n_char ) s_stat.n_char--;
    __result_str_accum(context, &s_stat);
  }
#endif /* DBSQL_ENABLE_STAT4 */
#ifndef DBSQL_DEBUG
  UNUSED_PARAMETER( argc );
#endif
}
static const func_def_t __stat_get_funcdef = {
  1+IS_STAT4,       /* n_arg */
  DBSQL_UTF8,     /* func_flags */
  0,               /* user_data */
  0,               /* next */
  __stat_get,         /* s_func */
  0,               /* finalize */
  0, 0,            /* value, inverse */
  "stat_get",      /* name */
  {0}
};

static void __call_stat_get(parser_t *parser, int reg_stat, int i_param, int reg_out){
#ifdef DBSQL_ENABLE_STAT4
  __vdbe_add_op2(parser->vdbe, OP_Integer, i_param, reg_stat+1);
#elif DBSQL_DEBUG
  assert( i_param==STAT_GET_STAT1 );
#else
  UNUSED_PARAMETER( i_param );
#endif
  assert( reg_out!=reg_stat && reg_out!=reg_stat+1 );
  __vdbe_add_function_call(parser, 0, reg_stat, reg_out, 1+IS_STAT4,
                             &__stat_get_funcdef, 0);
}

#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
/* Add a comment to the most recent VDBE opcode that is the name
** of the k-th column of the idx index.
*/
static void ANALYZE_VDBE_COMMENT_INDEX_WITH_COLUMN_NAME(
  vdbe_t *v,         /* Prepared statement under construction */
  index_t *idx,     /* index_t whose column is being loaded */
  int k            /* Which column index */
){
  int i;           /* index_t of column in the table */
  assert( k>=0 && k<idx->n_column );
  i = idx->columns[k];
  if( NEVER(i==XN_ROWID) ){
    VDBE_COMMENT((v,"%s.rowid",idx->name));
  }else if( i==XN_EXPR ){
    assert( idx->has_expr );
    VDBE_COMMENT((v,"%s.expr(%d)",idx->name, k));
  }else{
    VDBE_COMMENT((v,"%s.%s", idx->name, idx->table->cols[i].cn_name));
  }
}
#else
# define ANALYZE_VDBE_COMMENT_INDEX_WITH_COLUMN_NAME(a,b,c)
#endif /* DBSQL_DEBUG */

/*
** Generate code to do an analysis of all indices associated with
** a single table.
*/
static void __analyze_one_table(
  parser_t *parser,   /* Parser context */
  table_t *tab,     /* table_t whose indices are to be analyzed */
  index_t *only_idx, /* If not NULL, only analyze this one index */
  int i_stat_cur,    /* index_t of vdbe_cursor_t that writes the dbsql_stat1 table */
  int i_mem,        /* Available memory locations begin here */
  int i_tab         /* Next available cursor */
){
  DBSQL *db = parser->db;    /* Database handle */
  index_t *idx;                 /* An index to being analyzed */
  int i_idx_cur;                 /* Cursor open on index being analyzed */
  int i_tab_cur;                 /* table_t cursor */
  vdbe_t *v;                     /* The virtual machine being built up */
  int i;                       /* Loop counter */
  int j_zero_rows = -1;          /* Jump from here if number of rows is zero */
  int i_db;                     /* index_t of database containing tab */
  u_int8_t need_table_cnt = 1;         /* True to count the table */
  int reg_new_rowid = i_mem++;    /* Rowid for the inserted record */
  int reg_stat = i_mem++;        /* Register to hold stat_accum_t object */
  int reg_chng = i_mem++;        /* index_t of changed index field */
  int reg_rowid = i_mem++;       /* Rowid argument passed to stat_push() */
  int reg_temp = i_mem++;        /* Temporary use register */
  int reg_temp2 = i_mem++;       /* Second temporary use register */
  int reg_tabname = i_mem++;     /* Register containing table name */
  int reg_idxname = i_mem++;     /* Register containing index name */
  int reg_stat1 = i_mem++;       /* Value for the stat column of dbsql_stat1 */
  int reg_prev = i_mem;          /* MUST BE LAST (see below) */
#ifdef DBSQL_ENABLE_STAT4
  int do_once = 1;              /* Flag for a one-time computation */
#endif
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  table_t *stat1 = 0;
#endif

  __touch_register(parser, i_mem);
  assert( __no_temps_in_range(parser, reg_new_rowid, i_mem) );
  v = __get_vdbe(parser);
  if( v==0 || NEVER(tab==0) ){
    return;
  }
  if( !IS_ORDINARY_TABLE(tab) ){
    /* Do not gather statistics on views or virtual tables */
    return;
  }
  if( dbsql_strlike("dbsql\\_%", tab->name, '\\')==0 ){
    /* Do not gather statistics on system tables */
    return;
  }
  assert( __sm_holds_all_mutexes(db) );
  i_db = __schema_to_index(db, tab->schema);
  assert( i_db>=0 );
  assert( __schema_mutex_held(db, i_db, 0) );
#ifndef DBSQL_OMIT_AUTHORIZATION
  if( __auth_check(parser, DBSQL_ANALYZE, tab->name, 0,
      db->dbs[i_db].db_s_name ) ){
    return;
  }
#endif

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  if( db->pre_update_callback ){
    stat1 = (table_t*)__db_malloc_zero(db, sizeof(table_t) + 13);
    if( stat1==0 ) return;
    stat1->name = (char*)&stat1[1];
    memcpy(stat1->name, "dbsql_stat1", 13);
    stat1->n_col = 3;
    stat1->i_p_key = -1;
    __vdbe_add_op4(parser->vdbe, OP_Noop, 0, 0, 0,(char*)stat1,P4_DYNAMIC);
  }
#endif

  /* Establish a read-lock on the table at the shared-cache level. 
  ** Open a read-only cursor on the table. Also allocate a cursor number
  ** to use for scanning indexes (i_idx_cur). No index cursor is opened at
  ** this time though.  */
  __table_lock(parser, i_db, tab->tnum, 0, tab->name);
  i_tab_cur = i_tab++;
  i_idx_cur = i_tab++;
  parser->n_tab = MAX(parser->n_tab, i_tab);
  __open_table(parser, i_tab_cur, i_db, tab, OP_OpenRead);
  __vdbe_load_string(v, reg_tabname, tab->name);

  for(idx=tab->p_index; idx; idx=idx->next){
    int n_col;                     /* Number of columns in idx. "N" */
    int addr_goto_end;               /* Address of "OP_Rewind i_idx_cur" */
    int addr_next_row;              /* Address of "next_row:" */
    const char *idx_name;         /* Name of the index */
    int n_col_test;                 /* Number of columns to test for changes */

    if( only_idx && only_idx!=idx ) continue;
    if( idx->part_idx_where==0 ) need_table_cnt = 0;
    if( !HAS_ROWID(tab) && IS_PRIMARY_KEY_INDEX(idx) ){
      n_col = idx->n_key_col;
      idx_name = tab->name;
      n_col_test = n_col - 1;
    }else{
      n_col = idx->n_column;
      idx_name = idx->name;
      n_col_test = idx->uniq_not_null ? idx->n_key_col-1 : n_col-1;
    }

    /* Populate the register containing the index name. */
    __vdbe_load_string(v, reg_idxname, idx_name);
    VDBE_COMMENT((v, "Analysis for %s.%s", tab->name, idx_name));

    /*
    ** Pseudo-code for loop that calls stat_push():
    **
    **   reg_chng = 0
    **   Rewind csr
    **   if eof(csr){
    **      stat_init() with count = 0;
    **      goto end_of_scan;
    **   }
    **   count()
    **   stat_init()
    **   goto chng_addr_0;
    **
    **  next_row:
    **   reg_chng = 0
    **   if( idx(0) != reg_prev(0) ) goto chng_addr_0
    **   reg_chng = 1
    **   if( idx(1) != reg_prev(1) ) goto chng_addr_1
    **   ...
    **   reg_chng = N
    **   goto chng_addr_N
    **
    **  chng_addr_0:
    **   reg_prev(0) = idx(0)
    **  chng_addr_1:
    **   reg_prev(1) = idx(1)
    **  ...
    **
    **  end_distinct_test:
    **   reg_rowid = idx(rowid)
    **   stat_push(P, reg_chng, reg_rowid)
    **   Next csr
    **   if !eof(csr) goto next_row;
    **
    **  end_of_scan:
    */

    /* Make sure there are enough memory cells allocated to accommodate 
    ** the reg_prev array and a trailing rowid (the rowid slot is required
    ** when building a record to insert into the sample column of 
    ** the dbsql_stat4 table.  */
    __touch_register(parser, reg_prev+n_col_test);

    /* Open a read-only cursor on the index being analyzed. */
    assert( i_db==__schema_to_index(db, idx->schema) );
    __vdbe_add_op3(v, OP_OpenRead, i_idx_cur, idx->tnum, i_db);
    __vdbe_set_p4_key_info(parser, idx);
    VDBE_COMMENT((v, "%s", idx->name));

    /* Implementation of the following:
    **
    **   reg_chng = 0
    **   Rewind csr
    **   if eof(csr){
    **      stat_init() with count = 0;
    **      goto end_of_scan;
    **   }
    **   count()
    **   stat_init()
    **   goto chng_addr_0;
    */
    assert( reg_temp2==reg_stat+4 );
    __vdbe_add_op2(v, OP_Integer, db->n_analysis_limit, reg_temp2);

    /* Arguments to stat_init(): 
    **    (1) the number of columns in the index including the rowid
    **        (or for a WITHOUT ROWID table, the number of PK columns),
    **    (2) the number of columns in the key without the rowid/pk
    **    (3) estimated number of rows in the index. */
    __vdbe_add_op2(v, OP_Integer, n_col, reg_stat+1);
    assert( reg_rowid==reg_stat+2 );
    __vdbe_add_op2(v, OP_Integer, idx->n_key_col, reg_rowid);
    __vdbe_add_op3(v, OP_Count, i_idx_cur, reg_temp,
                      OPTIMIZATION_DISABLED(db, DBSQL_Stat4));
    __vdbe_add_function_call(parser, 0, reg_stat+1, reg_stat, 4,
                               &__stat_init_funcdef, 0);
    addr_goto_end = __vdbe_add_op1(v, OP_Rewind, i_idx_cur);
    VDBE_COVERAGE(v);

    __vdbe_add_op2(v, OP_Integer, 0, reg_chng);
    addr_next_row = __vdbe_current_addr(v);

    if( n_col_test>0 ){
      int end_distinct_test = __vdbe_make_label(parser);
      int *goto_chngs;               /* Array of jump instruction addresses */
      goto_chngs = __db_malloc_raw_nn(db, sizeof(int)*n_col_test);
      if( goto_chngs==0 ) continue;

      /*
      **  next_row:
      **   reg_chng = 0
      **   if( idx(0) != reg_prev(0) ) goto chng_addr_0
      **   reg_chng = 1
      **   if( idx(1) != reg_prev(1) ) goto chng_addr_1
      **   ...
      **   reg_chng = N
      **   goto end_distinct_test
      */
      __vdbe_add_op0(v, OP_Goto);
      addr_next_row = __vdbe_current_addr(v);
      if( n_col_test==1 && idx->n_key_col==1 && IS_UNIQUE_INDEX(idx) ){
        /* For a single-column UNIQUE index, once we have found a non-NULL
        ** row, we know that all the rest will be distinct, so skip 
        ** subsequent distinctness tests. */
        __vdbe_add_op2(v, OP_NotNull, reg_prev, end_distinct_test);
        VDBE_COVERAGE(v);
      }
      for(i=0; i<n_col_test; i++){
        char *coll = (char*)__locate_coll_seq(parser, idx->colls[i]);
        __vdbe_add_op2(v, OP_Integer, i, reg_chng);
        __vdbe_add_op3(v, OP_Column, i_idx_cur, i, reg_temp);
        ANALYZE_VDBE_COMMENT_INDEX_WITH_COLUMN_NAME(v,idx,i);
        goto_chngs[i] = 
        __vdbe_add_op4(v, OP_Ne, reg_temp, 0, reg_prev+i, coll, P4_COLLSEQ);
        __vdbe_change_p5(v, DBSQL_NULLEQ);
        VDBE_COVERAGE(v);
      }
      __vdbe_add_op2(v, OP_Integer, n_col_test, reg_chng);
      __vdbe_goto(v, end_distinct_test);
  
  
      /*
      **  chng_addr_0:
      **   reg_prev(0) = idx(0)
      **  chng_addr_1:
      **   reg_prev(1) = idx(1)
      **  ...
      */
      __vdbe_jump_here(v, addr_next_row-1);
      for(i=0; i<n_col_test; i++){
        __vdbe_jump_here(v, goto_chngs[i]);
        __vdbe_add_op3(v, OP_Column, i_idx_cur, i, reg_prev+i);
        ANALYZE_VDBE_COMMENT_INDEX_WITH_COLUMN_NAME(v,idx,i);
      }
      __vdbe_resolve_label(v, end_distinct_test);
      __dbsql_db_free(db, goto_chngs);
    }
  
    /*
    **  chng_addr_N:
    **   reg_rowid = idx(rowid)            // STAT4 only
    **   stat_push(P, reg_chng, reg_rowid)  // 3rd parameter STAT4 only
    **   Next csr
    **   if !eof(csr) goto next_row;
    */
#ifdef DBSQL_ENABLE_STAT4
    if( OPTIMIZATION_ENABLED(db, DBSQL_Stat4) ){
      assert( reg_rowid==(reg_stat+2) );
      if( HAS_ROWID(tab) ){
        __vdbe_add_op2(v, OP_IdxRowid, i_idx_cur, reg_rowid);
      }else{
        index_t *pk = __primary_key_index(idx->table);
        int j, k, reg_key;
        reg_key = __get_temp_range(parser, pk->n_key_col);
        for(j=0; j<pk->n_key_col; j++){
          k = __table_column_to_index(idx, pk->columns[j]);
          assert( k>=0 && k<idx->n_column );
          __vdbe_add_op3(v, OP_Column, i_idx_cur, k, reg_key+j);
          ANALYZE_VDBE_COMMENT_INDEX_WITH_COLUMN_NAME(v,idx,k);
        }
        __vdbe_add_op3(v, OP_MakeRecord, reg_key, pk->n_key_col, reg_rowid);
        __release_temp_range(parser, reg_key, pk->n_key_col);
      }
    }
#endif
    assert( reg_chng==(reg_stat+1) );
    {
      __vdbe_add_function_call(parser, 1, reg_stat, reg_temp, 2+IS_STAT4,
                                 &__stat_push_funcdef, 0);
      if( db->n_analysis_limit ){
        int j1, j2, j3;
        j1 = __vdbe_add_op1(v, OP_IsNull, reg_temp); VDBE_COVERAGE(v);
        j2 = __vdbe_add_op1(v, OP_If, reg_temp); VDBE_COVERAGE(v);
        j3 = __vdbe_add_op4_int(v, OP_SeekGT, i_idx_cur, 0, reg_prev, 1);
        VDBE_COVERAGE(v);
        __vdbe_jump_here(v, j1);
        __vdbe_add_op2(v, OP_Next, i_idx_cur, addr_next_row); VDBE_COVERAGE(v);
        __vdbe_jump_here(v, j2);
        __vdbe_jump_here(v, j3);
      }else{
        __vdbe_add_op2(v, OP_Next, i_idx_cur, addr_next_row); VDBE_COVERAGE(v);
      }
    }

    /* Add the entry to the stat1 table. */
    if( idx->part_idx_where ){
      /* Partial indexes might get a zero-entry in dbsql_stat1.  But
      ** an empty table is omitted from dbsql_stat1. */
      __vdbe_jump_here(v, addr_goto_end);
      addr_goto_end = 0;
    }
    __call_stat_get(parser, reg_stat, STAT_GET_STAT1, reg_stat1);
    assert( "BBB"[0]==DBSQL_AFF_TEXT );
    __vdbe_add_op4(v, OP_MakeRecord, reg_tabname, 3, reg_temp, "BBB", 0);
    __vdbe_add_op2(v, OP_NewRowid, i_stat_cur, reg_new_rowid);
    __vdbe_add_op3(v, OP_Insert, i_stat_cur, reg_temp, reg_new_rowid);
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
    __vdbe_change_p4(v, -1, (char*)stat1, P4_TABLE);
#endif
    __vdbe_change_p5(v, OPFLAG_APPEND);

    /* Add the entries to the stat4 table. */
#ifdef DBSQL_ENABLE_STAT4
    if( OPTIMIZATION_ENABLED(db, DBSQL_Stat4) && db->n_analysis_limit==0 ){
      int reg_eq = reg_stat1;
      int reg_lt = reg_stat1+1;
      int reg_d_lt = reg_stat1+2;
      int reg_sample = reg_stat1+3;
      int reg_col = reg_stat1+4;
      int reg_sample_rowid = reg_col + n_col;
      int addr_next;
      int addr_is_null;
      u_int8_t seek_op = HAS_ROWID(tab) ? OP_NotExists : OP_NotFound;

      /* No STAT4 data is generated if the number of rows is zero */
      if( addr_goto_end==0 ){
        __vdbe_add_op2(v, OP_Cast, reg_stat1, DBSQL_AFF_INTEGER);
        addr_goto_end = __vdbe_add_op1(v, OP_IfNot, reg_stat1);
        VDBE_COVERAGE(v);
      }

      if( do_once ){
        int mx_col = n_col;
        index_t *x;

        /* Compute the maximum number of columns in any index */
        for(x=tab->p_index; x; x=x->next){
          int n_col_x;                     /* Number of columns in x */
          if( !HAS_ROWID(tab) && IS_PRIMARY_KEY_INDEX(x) ){
            n_col_x = x->n_key_col;
          }else{
            n_col_x = x->n_column;
          }
          if( n_col_x>mx_col ) mx_col = n_col_x;
        }

        /* Allocate space to compute results for the largest index */
        __touch_register(parser, reg_col+mx_col);
        do_once = 0;
#ifdef DBSQL_DEBUG
        /* Verify that the call to __clear_temp_reg_cache() below
        ** really is needed.
        ** https://sqlite.org/forum/forumpost/83cb4a95a0 (2023-03-25)
        */
        testcase( !__no_temps_in_range(parser, reg_eq, reg_col+mx_col) );
#endif
        __clear_temp_reg_cache(parser);  /* tag-20230325-1 */
        assert( __no_temps_in_range(parser, reg_eq, reg_col+mx_col) );
      }
      assert( __no_temps_in_range(parser, reg_eq, reg_col+n_col) );

      addr_next = __vdbe_current_addr(v);
      __call_stat_get(parser, reg_stat, STAT_GET_ROWID, reg_sample_rowid);
      addr_is_null = __vdbe_add_op1(v, OP_IsNull, reg_sample_rowid);
      VDBE_COVERAGE(v);
      __call_stat_get(parser, reg_stat, STAT_GET_NEQ, reg_eq);
      __call_stat_get(parser, reg_stat, STAT_GET_NLT, reg_lt);
      __call_stat_get(parser, reg_stat, STAT_GET_NDLT, reg_d_lt);
      __vdbe_add_op4_int(v, seek_op, i_tab_cur, addr_next, reg_sample_rowid, 0);
      VDBE_COVERAGE(v);
      for(i=0; i<n_col; i++){
        __expr_code_load_index_column(parser, idx, i_tab_cur, i, reg_col+i);
      }
      __vdbe_add_op3(v, OP_MakeRecord, reg_col, n_col, reg_sample);
      __vdbe_add_op3(v, OP_MakeRecord, reg_tabname, 6, reg_temp);
      __vdbe_add_op2(v, OP_NewRowid, i_stat_cur+1, reg_new_rowid);
      __vdbe_add_op3(v, OP_Insert, i_stat_cur+1, reg_temp, reg_new_rowid);
      __vdbe_add_op2(v, OP_Goto, 1, addr_next); /* P1==1 for end-of-loop */
      __vdbe_jump_here(v, addr_is_null);
    }
#endif /* DBSQL_ENABLE_STAT4 */

    /* End of analysis */
    if( addr_goto_end ) __vdbe_jump_here(v, addr_goto_end);
  }


  /* Create a single dbsql_stat1 entry containing NULL as the index
  ** name and the row count as the content.
  */
  if( only_idx==0 && need_table_cnt ){
    VDBE_COMMENT((v, "%s", tab->name));
    __vdbe_add_op2(v, OP_Count, i_tab_cur, reg_stat1);
    j_zero_rows = __vdbe_add_op1(v, OP_IfNot, reg_stat1); VDBE_COVERAGE(v);
    __vdbe_add_op2(v, OP_Null, 0, reg_idxname);
    assert( "BBB"[0]==DBSQL_AFF_TEXT );
    __vdbe_add_op4(v, OP_MakeRecord, reg_tabname, 3, reg_temp, "BBB", 0);
    __vdbe_add_op2(v, OP_NewRowid, i_stat_cur, reg_new_rowid);
    __vdbe_add_op3(v, OP_Insert, i_stat_cur, reg_temp, reg_new_rowid);
    __vdbe_change_p5(v, OPFLAG_APPEND);
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
    __vdbe_change_p4(v, -1, (char*)stat1, P4_TABLE);
#endif
    __vdbe_jump_here(v, j_zero_rows);
  }
}


/*
** Generate code that will cause the most recent index analysis to
** be loaded into internal hash tables where is can be used.
*/
static void __load_analysis(parser_t *parser, int i_db){
  vdbe_t *v = __get_vdbe(parser);
  if( v ){
    __vdbe_add_op1(v, OP_LoadAnalysis, i_db);
  }
}

/*
** Generate code that will do an analysis of an entire database
*/
static void __analyze_database(parser_t *parser, int i_db){
  DBSQL *db = parser->db;
  schema_t *schema = db->dbs[i_db].schema;    /* schema_t of database i_db */
  hash_elem_t *k;
  int i_stat_cur;
  int i_mem;
  int i_tab;

  __begin_write_operation(parser, 0, i_db);
  i_stat_cur = parser->n_tab;
  parser->n_tab += 3;
  __open_stat_table(parser, i_db, i_stat_cur, 0, 0);
  i_mem = parser->n_mem+1;
  i_tab = parser->n_tab;
  assert( __schema_mutex_held(db, i_db, 0) );
  for(k=dbsql_hash_first(&schema->tbl_hash); k; k=dbsql_hash_next(k)){
    table_t *tab = (table_t*)dbsql_hash_data(k);
    __analyze_one_table(parser, tab, 0, i_stat_cur, i_mem, i_tab);
#ifdef DBSQL_ENABLE_STAT4
    i_mem = __first_available_register(parser, i_mem);
#else
    assert( i_mem==__first_available_register(parser,i_mem) );
#endif
  }
  __load_analysis(parser, i_db);
}

/*
** Generate code that will do an analysis of a single table in
** a database.  If only_idx is not NULL then it is a single index
** in tab that should be analyzed.
*/
static void __analyze_table(parser_t *parser, table_t *tab, index_t *only_idx){
  int i_db;
  int i_stat_cur;

  assert( tab!=0 );
  assert( __sm_holds_all_mutexes(parser->db) );
  i_db = __schema_to_index(parser->db, tab->schema);
  __begin_write_operation(parser, 0, i_db);
  i_stat_cur = parser->n_tab;
  parser->n_tab += 3;
  if( only_idx ){
    __open_stat_table(parser, i_db, i_stat_cur, only_idx->name, "idx");
  }else{
    __open_stat_table(parser, i_db, i_stat_cur, tab->name, "tbl");
  }
  __analyze_one_table(parser, tab, only_idx, i_stat_cur,parser->n_mem+1,parser->n_tab);
  __load_analysis(parser, i_db);
}

/*
** Generate code for the ANALYZE command.  The parser calls this routine
** when it recognizes an ANALYZE command.
**
**        ANALYZE                            -- 1
**        ANALYZE  <database>                -- 2
**        ANALYZE  ?<database>.?<tablename>  -- 3
**
** Form 1 causes all indices in all attached databases to be analyzed.
** Form 2 analyzes all indices the single database named.
** Form 3 analyzes all indices associated with the named table.
*/
void __analyze(parser_t *parser, Token *name1, Token *name2){
  DBSQL *db = parser->db;
  int i_db;
  int i;
  char *z, *z_db;
  table_t *tab;
  index_t *idx;
  Token *table_name;
  vdbe_t *v;

  /* Read the database schema. If an error occurs, leave an error message
  ** and code in parser and return NULL. */
  assert( __sm_holds_all_mutexes(parser->db) );
  if( DBSQL_OK!=__read_schema(parser) ){
    return;
  }

  assert( name2!=0 || name1==0 );
  if( name1==0 ){
    /* Form 1:  Analyze everything */
    for(i=0; i<db->n_db; i++){
      if( i==1 ) continue;  /* Do not analyze the TEMP database */
      __analyze_database(parser, i);
    }
  }else if( name2->n==0 && (i_db = __find_db(db, name1))>=0 ){
    /* Analyze the schema named as the argument */
    __analyze_database(parser, i_db);
  }else{
    /* Form 3: Analyze the table or index named as an argument */
    i_db = __two_part_name(parser, name1, name2, &table_name);
    if( i_db>=0 ){
      z_db = name2->n ? db->dbs[i_db].db_s_name : 0;
      z = __name_from_token(db, table_name);
      if( z ){
        if( (idx = __find_index(db, z, z_db))!=0 ){
          __analyze_table(parser, idx->table, idx);
        }else if( (tab = __locate_table(parser, 0, z, z_db))!=0 ){
          __analyze_table(parser, tab, 0);
        }
        __dbsql_db_free(db, z);
      }
    }
  }
  if( db->n_sql_exec==0 && (v = __get_vdbe(parser))!=0 ){
    __vdbe_add_op0(v, OP_Expire);
  }
}

/*
** Used to pass information from the analyzer reader through to the
** callback routine.
*/
typedef struct analysis_info analysis_info;
struct analysis_info {
  DBSQL *db;
  const char *database;
};

/*
** The first argument points to a nul-terminated string containing a
** list of space separated integers. Read the first n_out of these into
** the array outs[].
*/
static void decode_int_array(
  char *int_array,       /* String containing int array to decode */
  int n_out,              /* Number of slots in outs[] */
  rowcnt_t *outs,         /* Store integers here */
  log_est_t *logs,          /* Or, if outs==0, here */
  index_t *p_index          /* Handle extra flags for this index, if not NULL */
){
  char *z = int_array;
  int c;
  int i;
  rowcnt_t v;

#ifdef DBSQL_ENABLE_STAT4
  if( z==0 ) z = "";
#else
  assert( z!=0 );
#endif
  for(i=0; *z && i<n_out; i++){
    v = 0;
    while( (c=z[0])>='0' && c<='9' ){
      v = v*10 + c - '0';
      z++;
    }
#ifdef DBSQL_ENABLE_STAT4
    if( outs ) outs[i] = v;
    if( logs ) logs[i] = __log_est(v);
#else
    assert( outs==0 );
    UNUSED_PARAMETER(outs);
    assert( logs!=0 );
    logs[i] = __log_est(v);
#endif
    if( *z==' ' ) z++;
  }
#ifndef DBSQL_ENABLE_STAT4
  assert( p_index!=0 ); {
#else
  if( p_index ){
#endif
    p_index->unordered = 0;
    p_index->no_skip_scan = 0;
    while( z[0] ){
      if( dbsql_strglob("unordered*", z)==0 ){
        p_index->unordered = 1;
      }else if( dbsql_strglob("sz=[0-9]*", z)==0 ){
        int sz = __atoi(z+3);
        if( sz<2 ) sz = 2;
        p_index->sz_idx_row = __log_est(sz);
      }else if( dbsql_strglob("noskipscan*", z)==0 ){
        p_index->no_skip_scan = 1;
      }
#ifdef DBSQL_ENABLE_COSTMULT
      else if( dbsql_strglob("costmult=[0-9]*",z)==0 ){
        p_index->table->cost_mult = __log_est(__atoi(z+9));
      }
#endif
      while( z[0]!=0 && z[0]!=' ' ) z++;
      while( z[0]==' ' ) z++;
    }
  }
}

/*
** This callback is invoked once for each index when reading the
** dbsql_stat1 table.  
**
**     argv[0] = name of the table
**     argv[1] = name of the index (might be NULL)
**     argv[2] = results of analysis - on integer for each column
**
** Entries for which argv[1]==NULL simply record the number of rows in
** the table.
*/
static int analysis_loader(void *data, int argc, char **argv, char **__not_used){
  analysis_info *info = (analysis_info*)data;
  index_t *p_index;
  table_t *table;
  const char *z;

  assert( argc==3 );
  UNUSED_PARAMETER2(__not_used, argc);

  if( argv==0 || argv[0]==0 || argv[2]==0 ){
    return 0;
  }
  table = __find_table(info->db, argv[0], info->database);
  if( table==0 ){
    return 0;
  }
  if( argv[1]==0 ){
    p_index = 0;
  }else if( dbsql_stricmp(argv[0],argv[1])==0 ){
    p_index = __primary_key_index(table);
  }else{
    p_index = __find_index(info->db, argv[1], info->database);
  }
  z = argv[2];

  if( p_index ){
    rowcnt_t *row_ests = 0;
    int n_col = p_index->n_key_col+1;
#ifdef DBSQL_ENABLE_STAT4
    /* index_t.row_ests may already be set here if there are duplicate 
    ** dbsql_stat1 entries for this index. In that case just clobber
    ** the old data with the new instead of allocating a new array.  */
    if( p_index->row_ests==0 ){
      p_index->row_ests = (rowcnt_t*)__malloc_zero(sizeof(rowcnt_t) * n_col);
      if( p_index->row_ests==0 ) __oom_fault(info->db);
    }
    row_ests = p_index->row_ests;
#endif
    p_index->unordered = 0;
    decode_int_array((char*)z, n_col, row_ests, p_index->row_log_ests, p_index);
    p_index->has_stat1 = 1;
    if( p_index->part_idx_where==0 ){
      table->n_row_log_est = p_index->row_log_ests[0];
      table->tab_flags |= TF_HasStat1;
    }
  }else{
    index_t fake_idx;
    fake_idx.sz_idx_row = table->sz_tab_row;
#ifdef DBSQL_ENABLE_COSTMULT
    fake_idx.table = table;
#endif
    decode_int_array((char*)z, 1, 0, &table->n_row_log_est, &fake_idx);
    table->sz_tab_row = fake_idx.sz_idx_row;
    table->tab_flags |= TF_HasStat1;
  }

  return 0;
}

/*
** If the index_t.samples variable is not NULL, delete the samples[] array
** and its contents.
*/
void __delete_index_samples(DBSQL *db, index_t *idx){
  assert( db!=0 );
  assert( idx!=0 );
#ifdef DBSQL_ENABLE_STAT4
  if( idx->samples ){
    int j;
    for(j=0; j<idx->n_sample; j++){
      index_sample_t *p = &idx->samples[j];
      __dbsql_db_free(db, p->p);
    }
    __dbsql_db_free(db, idx->samples);
  }
  if( db->pn_bytes_freed==0 ){
    idx->n_sample = 0;
    idx->samples = 0;
  }
#else
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(idx);
#endif /* DBSQL_ENABLE_STAT4 */
}

#ifdef DBSQL_ENABLE_STAT4
/*
** Populate the idx->avg_eqs[] array based on the samples currently
** stored in idx->samples[]. 
*/
static void init_avg_eq(index_t *idx){
  if( idx ){
    index_sample_t *samples = idx->samples;
    index_sample_t *final = &samples[idx->n_sample-1];
    int i_col;
    int n_col = 1;
    if( idx->n_sample_col>1 ){
      /* If this is stat4 data, then calculate avg_eqs[] values for all
      ** sample columns except the last. The last is always set to 1, as
      ** once the trailing PK fields are considered all index keys are
      ** unique.  */
      n_col = idx->n_sample_col-1;
      idx->avg_eqs[n_col] = 1;
    }
    for(i_col=0; i_col<n_col; i_col++){
      int n_sample = idx->n_sample;
      int i;                    /* Used to iterate through samples */
      rowcnt_t sum_eq = 0;        /* Sum of the n_eq values */
      rowcnt_t avg_eq = 0;
      rowcnt_t n_row;             /* Number of rows in index */
      int64_t n_sum100 = 0;          /* Number of terms contributing to sum_eq */
      int64_t n_dist100;             /* Number of distinct values in index */

      if( !idx->row_ests || i_col>=idx->n_key_col || idx->row_ests[i_col+1]==0 ){
        n_row = final->an_lt[i_col];
        n_dist100 = (int64_t)100 * final->an_d_lt[i_col];
        n_sample--;
      }else{
        n_row = idx->row_ests[0];
        n_dist100 = ((int64_t)100 * idx->row_ests[0]) / idx->row_ests[i_col+1];
      }
      idx->n_row_est0 = n_row;

      /* Set n_sum to the number of distinct (i_col+1) field prefixes that
      ** occur in the stat4 table for this index. Set sum_eq to the sum of 
      ** the n_eq values for column i_col for the same set (adding the value 
      ** only once where there exist duplicate prefixes).  */
      for(i=0; i<n_sample; i++){
        if( i==(idx->n_sample-1)
         || samples[i].an_d_lt[i_col]!=samples[i+1].an_d_lt[i_col] 
        ){
          sum_eq += samples[i].an_eq[i_col];
          n_sum100 += 100;
        }
      }

      if( n_dist100>n_sum100 && sum_eq<n_row ){
        avg_eq = ((int64_t)100 * (n_row - sum_eq))/(n_dist100 - n_sum100);
      }
      if( avg_eq==0 ) avg_eq = 1;
      idx->avg_eqs[i_col] = avg_eq;
    }
  }
}

/*
** Look up an index by name.  Or, if the name of a WITHOUT ROWID table
** is supplied instead, find the PRIMARY KEY index for that table.
*/
static index_t *find_index_or_primary_key(
  DBSQL *db,
  const char *name,
  const char *db
){
  index_t *idx = __find_index(db, name, db);
  if( idx==0 ){
    table_t *tab = __find_table(db, name, db);
    if( tab && !HAS_ROWID(tab) ) idx = __primary_key_index(tab);
  }
  return idx;
}

/*
** Load the content from either the dbsql_stat4
** into the relevant index_t.samples[] arrays.
**
** Arguments sql1 and sql2 must point to SQL statements that return
** data equivalent to the following:
**
**    sql1: SELECT idx,count(*) FROM %Q.dbsql_stat4 GROUP BY idx
**    sql2: SELECT idx,neq,nlt,ndlt,sample FROM %Q.dbsql_stat4
**
** where %Q is replaced with the database name before the SQL is executed.
*/
static int load_stat_tbl(
  DBSQL *db,                  /* Database handle */
  const char *sql1,            /* SQL statement 1 (see above) */
  const char *sql2,            /* SQL statement 2 (see above) */
  const char *db               /* Database name (e.g. "main") */
){
  int rc;                       /* Result codes from subroutines */
  dbsql_stmt_t *stmt = 0;      /* An SQL statement being run */
  char *sql;                   /* Text of the SQL statement */
  index_t *prev_idx = 0;          /* Previous index in the loop */
  index_sample_t *sample;         /* A slot in idx->samples[] */

  assert( db->lookaside.disable );
  sql = __m_printf(db, sql1, db);
  if( !sql ){
    return DBSQL_NOMEM_BKPT;
  }
  rc = dbsql_prepare(db, sql, -1, &stmt, 0);
  __dbsql_db_free(db, sql);
  if( rc ) return rc;

  while( dbsql_step(stmt)==DBSQL_ROW ){
    int n_idx_col = 1;              /* Number of columns in stat4 records */

    char *z_index;    /* index_t name */
    index_t *idx;     /* Pointer to the index object */
    int n_sample;     /* Number of samples */
    int64_t n_byte;       /* Bytes of space required */
    int64_t i;           /* Bytes of space required */
    rowcnt_t *space; /* Available allocated memory space */
    u_int8_t *ptr;        /* Available memory as a u8 for easier manipulation */

    z_index = (char *)dbsql_column_text(stmt, 0);
    if( z_index==0 ) continue;
    n_sample = dbsql_column_int(stmt, 1);
    idx = find_index_or_primary_key(db, z_index, db);
    assert( idx==0 || idx->n_sample==0 );
    if( idx==0 ) continue;
    if( idx->samples!=0 ){
      /* The same index appears in dbsql_stat4 under multiple names */
      continue;
    }
    assert( !HAS_ROWID(idx->table) || idx->n_column==idx->n_key_col+1 );
    if( !HAS_ROWID(idx->table) && IS_PRIMARY_KEY_INDEX(idx) ){
      n_idx_col = idx->n_key_col;
    }else{
      n_idx_col = idx->n_column;
    }
    idx->n_sample_col = n_idx_col;
    idx->mx_sample = n_sample;
    n_byte = ROUND8(sizeof64(index_sample_t) * n_sample);
    n_byte += sizeof64(rowcnt_t) * n_idx_col * 3 * n_sample;
    n_byte += n_idx_col * sizeof64(rowcnt_t);   /* Space for index_t.avg_eqs[] */

    idx->samples = __db_malloc_zero(db, n_byte);
    if( idx->samples==0 ){
      dbsql_finalize(stmt);
      return DBSQL_NOMEM_BKPT;
    }
    ptr = (u_int8_t*)idx->samples;
    ptr += ROUND8(n_sample*sizeof64(idx->samples[0]));
    space = (rowcnt_t*)ptr;
    assert( EIGHT_BYTE_ALIGNMENT( space ) );
    idx->avg_eqs = space; space += n_idx_col;
    idx->table->tab_flags |= TF_HasStat4;
    for(i=0; i<n_sample; i++){
      idx->samples[i].an_eq = space; space += n_idx_col;
      idx->samples[i].an_lt = space; space += n_idx_col;
      idx->samples[i].an_d_lt = space; space += n_idx_col;
    }
    assert( ((u_int8_t*)space)-n_byte==(u_int8_t*)(idx->samples) );
  }
  rc = dbsql_finalize(stmt);
  if( rc ) return rc;

  sql = __m_printf(db, sql2, db);
  if( !sql ){
    return DBSQL_NOMEM_BKPT;
  }
  rc = dbsql_prepare(db, sql, -1, &stmt, 0);
  __dbsql_db_free(db, sql);
  if( rc ) return rc;

  while( dbsql_step(stmt)==DBSQL_ROW ){
    char *z_index;                 /* index_t name */
    index_t *idx;                  /* Pointer to the index object */
    int n_col = 1;                 /* Number of columns in index */

    z_index = (char *)dbsql_column_text(stmt, 0);
    if( z_index==0 ) continue;
    idx = find_index_or_primary_key(db, z_index, db);
    if( idx==0 ) continue;
    if( idx->n_sample>=idx->mx_sample ){
      /* Too many slots used because the same index appears in
      ** dbsql_stat4 using multiple names */
      continue;
    }
    /* This next condition is true if data has already been loaded from 
    ** the dbsql_stat4 table. */
    n_col = idx->n_sample_col;
    if( idx!=prev_idx ){
      init_avg_eq(prev_idx);
      prev_idx = idx;
    }
    sample = &idx->samples[idx->n_sample];
    decode_int_array((char*)dbsql_column_text(stmt,1),n_col,sample->an_eq,0,0);
    decode_int_array((char*)dbsql_column_text(stmt,2),n_col,sample->an_lt,0,0);
    decode_int_array((char*)dbsql_column_text(stmt,3),n_col,sample->an_d_lt,0,0);

    /* Take a copy of the sample. Add 8 extra 0x00 bytes the end of the buffer.
    ** This is in case the sample record is corrupted. In that case, the
    ** __vdbe_record_compare() may read up to two varints past the
    ** end of the allocated buffer before it realizes it is dealing with
    ** a corrupt record.  Or it might try to read a large integer from the
    ** buffer.  In any case, eight 0x00 bytes prevents this from causing
    ** a buffer overread.  */
    sample->n = dbsql_column_bytes(stmt, 4);
    sample->p = __db_malloc_zero(db, sample->n + 8);
    if( sample->p==0 ){
      dbsql_finalize(stmt);
      return DBSQL_NOMEM_BKPT;
    }
    if( sample->n ){
      memcpy(sample->p, dbsql_column_blob(stmt, 4), sample->n);
    }
    idx->n_sample++;
  }
  rc = dbsql_finalize(stmt);
  if( rc==DBSQL_OK ) init_avg_eq(prev_idx);
  return rc;
}

/*
** Load content from the dbsql_stat4 table into 
** the index_t.samples[] arrays of all indices.
*/
static int load_stat4(DBSQL *db, const char *db){
  int rc = DBSQL_OK;             /* Result codes from subroutines */
  const table_t *stat4;

  assert( db->lookaside.disable );
  if( OPTIMIZATION_ENABLED(db, DBSQL_Stat4)
   && (stat4 = __find_table(db, "dbsql_stat4", db))!=0
   && IS_ORDINARY_TABLE(stat4)
  ){
    rc = load_stat_tbl(db,
      "SELECT idx,count(*) FROM %Q.dbsql_stat4 GROUP BY idx COLLATE nocase", 
      "SELECT idx,neq,nlt,ndlt,sample FROM %Q.dbsql_stat4",
      db
    );
  }
  return rc;
}
#endif /* DBSQL_ENABLE_STAT4 */

/*
** Load the content of the dbsql_stat1 and dbsql_stat4 tables. The
** contents of dbsql_stat1 are used to populate the index_t.row_ests[]
** arrays. The contents of dbsql_stat4 are used to populate the
** index_t.samples[] arrays.
**
** If the dbsql_stat1 table is not present in the database, DBSQL_ERROR
** is returned. In this case, even if DBSQL_ENABLE_STAT4 was defined 
** during compilation and the dbsql_stat4 table is present, no data is 
** read from it.
**
** If DBSQL_ENABLE_STAT4 was defined during compilation and the 
** dbsql_stat4 table is not present in the database, DBSQL_ERROR is
** returned. However, in this case, data is read from the dbsql_stat1
** table (if it is present) before returning.
**
** If an OOM error occurs, this function always sets db->malloc_failed.
** This means if the caller does not care about other errors, the return
** code may be ignored.
*/
int __analysis_load(DBSQL *db, int i_db){
  analysis_info s_info;
  hash_elem_t *i;
  char *sql;
  int rc = DBSQL_OK;
  schema_t *schema = db->dbs[i_db].schema;
  const table_t *stat1;

  assert( i_db>=0 && i_db<db->n_db );
  assert( db->dbs[i_db].bt!=0 );

  /* Clear any prior statistics */
  assert( __schema_mutex_held(db, i_db, 0) );
  for(i=dbsql_hash_first(&schema->tbl_hash); i; i=dbsql_hash_next(i)){
    table_t *tab = dbsql_hash_data(i);
    tab->tab_flags &= ~TF_HasStat1;
  }
  for(i=dbsql_hash_first(&schema->idx_hash); i; i=dbsql_hash_next(i)){
    index_t *idx = dbsql_hash_data(i);
    idx->has_stat1 = 0;
#ifdef DBSQL_ENABLE_STAT4
    __delete_index_samples(db, idx);
    idx->samples = 0;
#endif
  }

  /* Load new statistics out of the dbsql_stat1 table */
  s_info.db = db;
  s_info.database = db->dbs[i_db].db_s_name;
  if( (stat1 = __find_table(db, "dbsql_stat1", s_info.database))
   && IS_ORDINARY_TABLE(stat1)
  ){
    sql = __m_printf(db, 
        "SELECT tbl,idx,stat FROM %Q.dbsql_stat1", s_info.database);
    if( sql==0 ){
      rc = DBSQL_NOMEM_BKPT;
    }else{
      rc = dbsql_exec(db, sql, analysis_loader, &s_info, 0);
      __dbsql_db_free(db, sql);
    }
  }

  /* Set appropriate defaults on all indexes not in the dbsql_stat1 table */
  assert( __schema_mutex_held(db, i_db, 0) );
  for(i=dbsql_hash_first(&schema->idx_hash); i; i=dbsql_hash_next(i)){
    index_t *idx = dbsql_hash_data(i);
    if( !idx->has_stat1 ) __default_row_est(idx);
  }

  /* Load the statistics from the dbsql_stat4 table. */
#ifdef DBSQL_ENABLE_STAT4
  if( rc==DBSQL_OK ){
    DISABLE_LOOKASIDE;
    rc = load_stat4(db, s_info.database);
    ENABLE_LOOKASIDE;
  }
  for(i=dbsql_hash_first(&schema->idx_hash); i; i=dbsql_hash_next(i)){
    index_t *idx = dbsql_hash_data(i);
    dbsql_free(idx->row_ests);
    idx->row_ests = 0;
  }
#endif

  if( rc==DBSQL_NOMEM ){
    __oom_fault(db);
  }
  return rc;
}


#endif /* DBSQL_OMIT_ANALYZE */
