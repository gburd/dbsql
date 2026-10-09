/*
** 2009 March 3
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
** This file contains the implementation of the dbsql_unlock_notify()
** API method and its associated functionality.
*/
#include "dbsql_int.h"
#include "btree_int.h"

/* Omit this entire file if DBSQL_ENABLE_UNLOCK_NOTIFY is not defined. */
#ifdef DBSQL_ENABLE_UNLOCK_NOTIFY

/*
** Public interfaces:
**
**   __connection_blocked()
**   __connection_unlocked()
**   __connection_closed()
**   dbsql_unlock_notify()
*/

#define ASSERT_MUTEX_HELD() \
  assert( dbsql_mutex_held(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN)) )

/*
** Head of a linked list of all DBSQL objects created by this process
** for which either DBSQL.blocking_connection or DBSQL.unlock_connection
** is not NULL. This variable may only accessed while the STATIC_MAIN
** mutex is held.
*/
static DBSQL *DBSQL_WSD __blocked_list = 0;

#ifndef NDEBUG
/*
** This function is a complex assert() that verifies the following 
** properties of the blocked connections list:
**
**   1) Each entry in the list has a non-NULL value for either 
**      unlock_connection or blocking_connection, or both.
**
**   2) All entries in the list that share a common value for 
**      unlock_notify are grouped together.
**
**   3) If the argument db is not NULL, then none of the entries in the
**      blocked connections list have unlock_connection or blocking_connection
**      set to db. This is used when closing connection db.
*/
static void CHECK_LIST_PROPERTIES(DBSQL *db){
  DBSQL *p;
  for(p=__blocked_list; p; p=p->next_blocked){
    int seen = 0;
    DBSQL *p2;

    /* Verify property (1) */
    assert( p->unlock_connection || p->blocking_connection );

    /* Verify property (2) */
    for(p2=__blocked_list; p2!=p; p2=p2->next_blocked){
      if( p2->unlock_notify==p->unlock_notify ) seen = 1;
      assert( p2->unlock_notify==p->unlock_notify || !seen );
      assert( db==0 || p->unlock_connection!=db );
      assert( db==0 || p->blocking_connection!=db );
    }
  }
}
#else
# define CHECK_LIST_PROPERTIES(x)
#endif

/*
** Remove connection db from the blocked connections list. If connection
** db is not currently a part of the list, this function is a no-op.
*/
static void __remove_from_blocked_list(DBSQL *db){
  DBSQL **pp;
  ASSERT_MUTEX_HELD();
  for(pp=&__blocked_list; *pp; pp = &(*pp)->next_blocked){
    if( *pp==db ){
      *pp = (*pp)->next_blocked;
      break;
    }
  }
}

/*
** Add connection db to the blocked connections list. It is assumed
** that it is not already a part of the list.
*/
static void __add_to_blocked_list(DBSQL *db){
  DBSQL **pp;
  ASSERT_MUTEX_HELD();
  for(
    pp=&__blocked_list; 
    *pp && (*pp)->unlock_notify!=db->unlock_notify; 
    pp=&(*pp)->next_blocked
  );
  db->next_blocked = *pp;
  *pp = db;
}

/*
** Obtain the STATIC_MAIN mutex.
*/
static void __enter_mutex(void){
  dbsql_mutex_enter(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN));
  CHECK_LIST_PROPERTIES(0);
}

/*
** Release the STATIC_MAIN mutex.
*/
static void __leave_mutex(void){
  ASSERT_MUTEX_HELD();
  CHECK_LIST_PROPERTIES(0);
  dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN));
}

/*
** Register an unlock-notify callback.
**
** This is called after connection "db" has attempted some operation
** but has received an DBSQL_LOCKED error because another connection
** (call it other) in the same process was busy using the same shared
** cache.  other is found by looking at db->blocking_connection.
**
** If there is no blocking connection, the callback is invoked immediately,
** before this routine returns.
**
** If other is already blocked on db, then report DBSQL_LOCKED, to indicate
** a deadlock.
**
** Otherwise, make arrangements to invoke notify when other drops
** its locks.
**
** Each call to this routine overrides any prior callbacks registered
** on the same "db".  If notify==0 then any prior callbacks are immediately
** cancelled.
*/
int dbsql_unlock_notify(
  DBSQL *db,
  void (*notify)(void **, int),
  void *arg
){
  int rc = DBSQL_OK;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  __enter_mutex();

  if( notify==0 ){
    __remove_from_blocked_list(db);
    db->blocking_connection = 0;
    db->unlock_connection = 0;
    db->unlock_notify = 0;
    db->unlock_arg = 0;
  }else if( 0==db->blocking_connection ){
    /* The blocking transaction has been concluded. Or there never was a 
    ** blocking transaction. In either case, invoke the notify callback
    ** immediately. 
    */
    notify(&arg, 1);
  }else{
    DBSQL *p;

    for(p=db->blocking_connection; p && p!=db; p=p->unlock_connection){}
    if( p ){
      rc = DBSQL_LOCKED;              /* Deadlock detected. */
    }else{
      db->unlock_connection = db->blocking_connection;
      db->unlock_notify = notify;
      db->unlock_arg = arg;
      __remove_from_blocked_list(db);
      __add_to_blocked_list(db);
    }
  }

  __leave_mutex();
  assert( !db->malloc_failed );
  __error_with_msg(db, rc, (rc?"database is deadlocked":0));
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** This function is called while stepping or preparing a statement 
** associated with connection db. The operation will return DBSQL_LOCKED
** to the user because it requires a lock that will not be available
** until connection blocker concludes its current transaction.
*/
void __connection_blocked(DBSQL *db, DBSQL *blocker){
  __enter_mutex();
  if( db->blocking_connection==0 && db->unlock_connection==0 ){
    __add_to_blocked_list(db);
  }
  db->blocking_connection = blocker;
  __leave_mutex();
}

/*
** This function is called when
** the transaction opened by database db has just finished. Locks held 
** by database connection db have been released.
**
** This function loops through each entry in the blocked connections
** list and does the following:
**
**   1) If the DBSQL.blocking_connection member of a list entry is
**      set to db, then set blocking_connection=0.
**
**   2) If the DBSQL.unlock_connection member of a list entry is
**      set to db, then invoke the configured unlock-notify callback and
**      set unlock_connection=0.
**
**   3) If the two steps above mean that blocking_connection==0 and
**      unlock_connection==0, remove the entry from the blocked connections
**      list.
*/
void __connection_unlocked(DBSQL *db){
  void (*unlock_notify)(void **, int) = 0; /* Unlock-notify cb to invoke */
  int n_arg = 0;                            /* Number of entries in args[] */
  DBSQL **pp;                            /* Iterator variable */
  void **args;               /* Arguments to the unlock callback */
  void **dyns = 0;           /* Dynamically allocated space for args[] */
  void *statics[16];         /* Starter space for args[].  No malloc required */

  args = statics;
  __enter_mutex();         /* Enter STATIC_MAIN mutex */

  /* This loop runs once for each entry in the blocked-connections list. */
  for(pp=&__blocked_list; *pp; /* no-op */ ){
    DBSQL *p = *pp;

    /* Step 1. */
    if( p->blocking_connection==db ){
      p->blocking_connection = 0;
    }

    /* Step 2. */
    if( p->unlock_connection==db ){
      assert( p->unlock_notify );
      if( p->unlock_notify!=unlock_notify && n_arg!=0 ){
        unlock_notify(args, n_arg);
        n_arg = 0;
      }

      __begin_benign_malloc();
      assert( args==dyns || (dyns==0 && args==statics) );
      assert( n_arg<=(int)ARRAY_SIZE(statics) || args==dyns );
      if( (!dyns && n_arg==(int)ARRAY_SIZE(statics))
       || (dyns && n_arg==(int)(__malloc_size(dyns)/sizeof(void*)))
      ){
        /* The args[] array needs to grow. */
        void **p_new = (void **)__dbsql_malloc(n_arg*sizeof(void *)*2);
        if( p_new ){
          memcpy(p_new, args, n_arg*sizeof(void *));
          dbsql_free(dyns);
          dyns = args = p_new;
        }else{
          /* This occurs when the array of context pointers that need to
          ** be passed to the unlock-notify callback is larger than the
          ** statics[] array allocated on the stack and the attempt to 
          ** allocate a larger array from the heap has failed.
          **
          ** This is a difficult situation to handle. returning_t an error
          ** code to the caller is insufficient, as even if an error code
          ** is returned the transaction on connection db will still be
          ** closed and the unlock-notify callbacks on blocked connections
          ** will go unissued. This might cause the application to wait
          ** indefinitely for an unlock-notify callback that will never 
          ** arrive.
          **
          ** Instead, invoke the unlock-notify callback with the context
          ** array already accumulated. We can then clear the array and
          ** begin accumulating any further context pointers without 
          ** requiring any dynamic allocation. This is sub-optimal because
          ** it means that instead of one callback with a large array of
          ** context pointers the application will receive two or more
          ** callbacks with smaller arrays of context pointers, which will
          ** reduce the applications ability to prioritize multiple 
          ** connections. But it is the best that can be done under the
          ** circumstances.
          */
          unlock_notify(args, n_arg);
          n_arg = 0;
        }
      }
      __end_benign_malloc();

      args[n_arg++] = p->unlock_arg;
      unlock_notify = p->unlock_notify;
      p->unlock_connection = 0;
      p->unlock_notify = 0;
      p->unlock_arg = 0;
    }

    /* Step 3. */
    if( p->blocking_connection==0 && p->unlock_connection==0 ){
      /* Remove connection p from the blocked connections list. */
      *pp = p->next_blocked;
      p->next_blocked = 0;
    }else{
      pp = &p->next_blocked;
    }
  }

  if( n_arg!=0 ){
    unlock_notify(args, n_arg);
  }
  dbsql_free(dyns);
  __leave_mutex();         /* Leave STATIC_MAIN mutex */
}

/*
** This is called when the database connection passed as an argument is 
** being closed. The connection is removed from the blocked list.
*/
void __connection_closed(DBSQL *db){
  __connection_unlocked(db);
  __enter_mutex();
  __remove_from_blocked_list(db);
  CHECK_LIST_PROPERTIES(db);
  __leave_mutex();
}
#endif
