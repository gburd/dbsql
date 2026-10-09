/*
** 2008 June 18
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
** This module implements the dbsql_status() interface and related
** functionality.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

/*
** Variables in which to record status information.
*/
#if DBSQL_PTRSIZE>4
typedef int64_t __stat_value_type;
#else
typedef u_int32_t __stat_value_type;
#endif
typedef struct __stat_type __stat_type;
static DBSQL_WSD struct __stat_type {
  __stat_value_type now_value[10];  /* Current value */
  __stat_value_type mx_value[10];   /* Maximum value */
} __stat = { {0,}, {0,} };

/*
** Elements of __stat[] are protected by either the memory allocator
** mutex, or by the pcache1 mutex.  The following array determines which.
*/
static const char __stat_mutex[] = {
  0,  /* DBSQL_STATUS_MEMORY_USED */
  1,  /* DBSQL_STATUS_PAGECACHE_USED */
  1,  /* DBSQL_STATUS_PAGECACHE_OVERFLOW */
  0,  /* DBSQL_STATUS_SCRATCH_USED */
  0,  /* DBSQL_STATUS_SCRATCH_OVERFLOW */
  0,  /* DBSQL_STATUS_MALLOC_SIZE */
  0,  /* DBSQL_STATUS_PARSER_STACK */
  1,  /* DBSQL_STATUS_PAGECACHE_SIZE */
  0,  /* DBSQL_STATUS_SCRATCH_SIZE */
  0,  /* DBSQL_STATUS_MALLOC_COUNT */
};


/* The "__wsd_stat" macro will resolve to the status information
** state vector.  If writable static data is unsupported on the target,
** we have to locate the state vector at run-time.  In the more common
** case where writable static data is supported, __wsd_stat can refer directly
** to the "__stat" state vector declared above.
*/
#ifdef DBSQL_OMIT_WSD
# define WSD_STAT_INIT  __stat_type *x = &GLOBAL(__stat_type,__stat)
# define __wsd_stat x[0]
#else
# define WSD_STAT_INIT
# define __wsd_stat __stat
#endif

/*
** Return the current value of a status parameter.  The caller must
** be holding the appropriate mutex.
*/
int64_t __status_value(int op){
  WSD_STAT_INIT;
  assert( op>=0 && op<ARRAY_SIZE(__wsd_stat.now_value) );
  assert( op>=0 && op<ARRAY_SIZE(__stat_mutex) );
  assert( dbsql_mutex_held(__stat_mutex[op] ? __pcache1_mutex()
                                           : __malloc_mutex()) );
  return __wsd_stat.now_value[op];
}

/*
** Add N to the value of a status record.  The caller must hold the
** appropriate mutex.  (Locking is checked by assert()).
**
** The __status_up() routine can accept positive or negative values for N.
** The value of N is added to the current status value and the high-water
** mark is adjusted if necessary.
**
** The __status_down() routine lowers the current value by N.  The highwater
** mark is unchanged.  N must be non-negative for __status_down().
*/
void __status_up(int op, int N){
  WSD_STAT_INIT;
  assert( op>=0 && op<ARRAY_SIZE(__wsd_stat.now_value) );
  assert( op>=0 && op<ARRAY_SIZE(__stat_mutex) );
  assert( dbsql_mutex_held(__stat_mutex[op] ? __pcache1_mutex()
                                           : __malloc_mutex()) );
  __wsd_stat.now_value[op] += N;
  if( __wsd_stat.now_value[op]>__wsd_stat.mx_value[op] ){
    __wsd_stat.mx_value[op] = __wsd_stat.now_value[op];
  }
}
void __status_down(int op, int N){
  WSD_STAT_INIT;
  assert( N>=0 );
  assert( op>=0 && op<ARRAY_SIZE(__stat_mutex) );
  assert( dbsql_mutex_held(__stat_mutex[op] ? __pcache1_mutex()
                                           : __malloc_mutex()) );
  assert( op>=0 && op<ARRAY_SIZE(__wsd_stat.now_value) );
  __wsd_stat.now_value[op] -= N;
}

/*
** Adjust the highwater mark if necessary.
** The caller must hold the appropriate mutex.
*/
void __status_highwater(int op, int X){
  __stat_value_type newValue;
  WSD_STAT_INIT;
  assert( X>=0 );
  newValue = (__stat_value_type)X;
  assert( op>=0 && op<ARRAY_SIZE(__wsd_stat.now_value) );
  assert( op>=0 && op<ARRAY_SIZE(__stat_mutex) );
  assert( dbsql_mutex_held(__stat_mutex[op] ? __pcache1_mutex()
                                           : __malloc_mutex()) );
  assert( op==DBSQL_STATUS_MALLOC_SIZE
          || op==DBSQL_STATUS_PAGECACHE_SIZE
          || op==DBSQL_STATUS_PARSER_STACK );
  if( newValue>__wsd_stat.mx_value[op] ){
    __wsd_stat.mx_value[op] = newValue;
  }
}

/*
** Query status information.
*/
int dbsql_status64(
  int op,
  int64_t *current,
  int64_t *highwater,
  int reset_flag
){
  dbsql_mutex_t *mutex;
  WSD_STAT_INIT;
  if( op<0 || op>=ARRAY_SIZE(__wsd_stat.now_value) ){
    return DBSQL_MISUSE_BKPT;
  }
#ifdef DBSQL_ENABLE_API_ARMOR
  if( current==0 || highwater==0 ) return DBSQL_MISUSE_BKPT;
#endif
  mutex = __stat_mutex[op] ? __pcache1_mutex() : __malloc_mutex();
  dbsql_mutex_enter(mutex);
  *current = __wsd_stat.now_value[op];
  *highwater = __wsd_stat.mx_value[op];
  if( reset_flag ){
    __wsd_stat.mx_value[op] = __wsd_stat.now_value[op];
  }
  dbsql_mutex_leave(mutex);
  (void)mutex;  /* Prevent warning when DBSQL_THREADSAFE=0 */
  return DBSQL_OK;
}
int dbsql_status(int op, int *current, int *highwater, int reset_flag){
  int64_t i_cur = 0, i_hwtr = 0;
  int rc;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( current==0 || highwater==0 ) return DBSQL_MISUSE_BKPT;
#endif
  rc = dbsql_status64(op, &i_cur, &i_hwtr, reset_flag);
  if( rc==0 ){
    *current = (int)i_cur;
    *highwater = (int)i_hwtr;
  }
  return rc;
}

/*
** Return the number of lookaside_slot_t elements on the linked list
*/
static u_int32_t __count_lookaside_slots(lookaside_slot_t *p){
  u_int32_t cnt = 0;
  while( p ){
    p = p->next;
    cnt++;
  }
  return cnt;
}

/*
** Count the number of slots of lookaside memory that are outstanding
*/
int __lookaside_used(DBSQL *db, int *highwater){
  u_int32_t n_init = __count_lookaside_slots(db->lookaside.init);
  u_int32_t n_free = __count_lookaside_slots(db->lookaside.p_free);
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
  n_init += __count_lookaside_slots(db->lookaside.small_init);
  n_free += __count_lookaside_slots(db->lookaside.small_free);
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
  assert( db->lookaside.n_slot >= n_init+n_free );
  if( highwater ) *highwater = (int)(db->lookaside.n_slot - n_init);
  return (int)(db->lookaside.n_slot - (n_init+n_free));
}

/*
** Query status information for a single database connection
*/
int dbsql_db_status64(
  DBSQL *db,             /* The database connection whose status is desired */
  int op,                  /* Status verb */
  int64_t *current, /* Write current value here */
  int64_t *highwtr, /* Write high-water mark here */
  int reset_flag            /* Reset high-water mark if true */
){
  int rc = DBSQL_OK;   /* Return code */
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || current==0|| highwtr==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  switch( op ){
    case DBSQL_DBSTATUS_LOOKASIDE_USED: {
      int H = 0;
      *current = __lookaside_used(db, &H);
      *highwtr = H;
      if( reset_flag ){
        lookaside_slot_t *p = db->lookaside.p_free;
        if( p ){
          while( p->next ) p = p->next;
          p->next = db->lookaside.init;
          db->lookaside.init = db->lookaside.p_free;
          db->lookaside.p_free = 0;
        }
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
        p = db->lookaside.small_free;
        if( p ){
          while( p->next ) p = p->next;
          p->next = db->lookaside.small_init;
          db->lookaside.small_init = db->lookaside.small_free;
          db->lookaside.small_free = 0;
        }
#endif
      }
      break;
    }

    case DBSQL_DBSTATUS_LOOKASIDE_HIT:
    case DBSQL_DBSTATUS_LOOKASIDE_MISS_SIZE:
    case DBSQL_DBSTATUS_LOOKASIDE_MISS_FULL: {
      testcase( op==DBSQL_DBSTATUS_LOOKASIDE_HIT );
      testcase( op==DBSQL_DBSTATUS_LOOKASIDE_MISS_SIZE );
      testcase( op==DBSQL_DBSTATUS_LOOKASIDE_MISS_FULL );
      assert( (op-DBSQL_DBSTATUS_LOOKASIDE_HIT)>=0 );
      assert( (op-DBSQL_DBSTATUS_LOOKASIDE_HIT)<3 );
      *current = 0;
      *highwtr = db->lookaside.an_stat[op-DBSQL_DBSTATUS_LOOKASIDE_HIT];
      if( reset_flag ){
        db->lookaside.an_stat[op - DBSQL_DBSTATUS_LOOKASIDE_HIT] = 0;
      }
      break;
    }

    /* 
    ** Return an approximation for the amount of memory currently used
    ** by all pagers associated with the given database connection.  The
    ** highwater mark is meaningless and is returned as zero.
    */
    case DBSQL_DBSTATUS_CACHE_USED_SHARED:
    case DBSQL_DBSTATUS_CACHE_USED: {
      int64_t total_used = 0;
      int i;
      __sm_enter_all(db);
      for(i=0; i<db->n_db; i++){
        sm_t *bt = db->dbs[i].bt;
        if( bt ){
          pager_t *pager = __sm_pager(bt);
          int n_byte = __sm_pager_mem_used(pager);
          if( op==DBSQL_DBSTATUS_CACHE_USED_SHARED ){
            n_byte = n_byte / __sm_connection_count(bt);
          }
          total_used += n_byte;
        }
      }
      __sm_leave_all(db);
      *current = total_used;
      *highwtr = 0;
      break;
    }

    /*
    ** *current gets an accurate estimate of the amount of memory used
    ** to store the schema for all databases (main, temp, and any ATTACHed
    ** databases.  *highwtr is set to zero.
    */
    case DBSQL_DBSTATUS_SCHEMA_USED: {
      int i;          /* Used to iterate through schemas */
      int n_byte = 0;  /* Used to accumulate return value */

      __sm_enter_all(db);
      db->pn_bytes_freed = &n_byte;
      assert( db->lookaside.p_end==db->lookaside.true_end );
      db->lookaside.p_end = db->lookaside.p_start;
      for(i=0; i<db->n_db; i++){
        schema_t *schema = db->dbs[i].schema;
        if( ALWAYS(schema!=0) ){
          hash_elem_t *p;

          n_byte += __global_config.m.roundup(sizeof(hash_elem_t)) * (
              schema->tbl_hash.count 
            + schema->trig_hash.count
            + schema->idx_hash.count
            + schema->fkey_hash.count
          );
          n_byte += dbsql_msize(schema->tbl_hash.ht);
          n_byte += dbsql_msize(schema->trig_hash.ht);
          n_byte += dbsql_msize(schema->idx_hash.ht);
          n_byte += dbsql_msize(schema->fkey_hash.ht);

          for(p=dbsql_hash_first(&schema->trig_hash); p; p=dbsql_hash_next(p)){
            __delete_trigger(db, (trigger_t*)dbsql_hash_data(p));
          }
          for(p=dbsql_hash_first(&schema->tbl_hash); p; p=dbsql_hash_next(p)){
            __delete_table(db, (table_t *)dbsql_hash_data(p));
          }
        }
      }
      db->pn_bytes_freed = 0;
      db->lookaside.p_end = db->lookaside.true_end;
      __sm_leave_all(db);

      *highwtr = 0;
      *current = n_byte;
      break;
    }

    /*
    ** *current gets an accurate estimate of the amount of memory used
    ** to store all prepared statements.
    ** *highwtr is set to zero.
    */
    case DBSQL_DBSTATUS_STMT_USED: {
      struct vdbe_t *vdbe;         /* Used to iterate through VMs */
      int n_byte = 0;              /* Used to accumulate return value */

      db->pn_bytes_freed = &n_byte;
      assert( db->lookaside.p_end==db->lookaside.true_end );
      db->lookaside.p_end = db->lookaside.p_start;
      for(vdbe=db->vdbe; vdbe; vdbe=vdbe->v_next){
        __vdbe_delete(vdbe);
      }
      db->lookaside.p_end = db->lookaside.true_end;
      db->pn_bytes_freed = 0;

      *highwtr = 0;  /* IMP: R-64479-57858 */
      *current = n_byte;

      break;
    }

    /*
    ** Set *current to the total cache hits or misses encountered by all
    ** pagers the database handle is connected to. *highwtr is always set 
    ** to zero.
    */
    case DBSQL_DBSTATUS_CACHE_SPILL:
      op = DBSQL_DBSTATUS_CACHE_WRITE+1;
      /* no break */ deliberate_fall_through
    case DBSQL_DBSTATUS_CACHE_HIT:
    case DBSQL_DBSTATUS_CACHE_MISS:
    case DBSQL_DBSTATUS_CACHE_WRITE:{
      int i;
      u_int64_t n_ret = 0;
      assert( DBSQL_DBSTATUS_CACHE_MISS==DBSQL_DBSTATUS_CACHE_HIT+1 );
      assert( DBSQL_DBSTATUS_CACHE_WRITE==DBSQL_DBSTATUS_CACHE_HIT+2 );

      for(i=0; i<db->n_db; i++){
        if( db->dbs[i].bt ){
          pager_t *pager = __sm_pager(db->dbs[i].bt);
          __sm_pager_cache_stat(pager, op, reset_flag, &n_ret);
        }
      }
      *highwtr = 0; /* IMP: R-42420-56072 */
                       /* IMP: R-54100-20147 */
                       /* IMP: R-29431-39229 */
      *current = n_ret;
      break;
    }

    /* Set *current to the number of bytes that the db database connection
    ** has spilled to the filesystem in temporary files that could have been
    ** stored in memory, had sufficient memory been available.
    ** The *highwater is always set to zero.
    */
    case DBSQL_DBSTATUS_TEMPBUF_SPILL: {
      u_int64_t n_ret = 0;
      if( db->dbs[1].bt ){
        pager_t *pager = __sm_pager(db->dbs[1].bt);
        __sm_pager_cache_stat(pager, DBSQL_DBSTATUS_CACHE_WRITE,
                              reset_flag, &n_ret);
        n_ret *= __sm_get_page_size(db->dbs[1].bt);
      }
      n_ret += db->n_spill;
      if( reset_flag ) db->n_spill = 0;
      *highwtr = 0;
      *current = n_ret;
      break;
    }

    /* Set *current to non-zero if there are unresolved deferred foreign
    ** key constraints.  Set *current to zero if all foreign key constraints
    ** have been satisfied.  The *highwtr is always set to zero.
    */
    case DBSQL_DBSTATUS_DEFERRED_FKS: {
      *highwtr = 0;  /* IMP: R-11967-56545 */
      *current = db->n_deferred_imm_cons>0 || db->n_deferred_cons>0;
      break;
    }

    default: {
      rc = DBSQL_ERROR;
    }
  }
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** 32-bit variant of dbsql_db_status64()
*/
int dbsql_db_status(
  DBSQL *db,             /* The database connection whose status is desired */
  int op,                  /* Status verb */
  int *current,           /* Write current value here */
  int *highwtr,           /* Write high-water mark here */
  int reset_flag            /* Reset high-water mark if true */
){
  int64_t C = 0, H = 0;
  int rc;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || current==0|| highwtr==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  rc = dbsql_db_status64(db, op, &C, &H, reset_flag);
  if( rc==0 ){
    *current = C & 0x7fffffff;
    *highwtr = H & 0x7fffffff;
  }
  return rc;
}
