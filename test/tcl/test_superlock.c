/*
** 2010 November 19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Example code for obtaining an exclusive lock on an dbsql database
** file. This method is complicated, but works for both WAL and rollback
** mode database files. The interface to the example code in this file 
** consists of the following two functions:
**
**   sqlite3demo_superlock()
**   sqlite3demo_superunlock()
*/

#include "dbsql.h"
#include <string.h>               /* memset(), strlen() */
#include <assert.h>               /* assert() */

/*
** A structure to collect a busy-handler callback and argument and a count
** of the number of times it has been invoked.
*/
struct superlock_busy_t {
  int (*__x_busy)(void*,int);        /* Pointer to busy-handler function */
  void *busy_arg;                 /* First arg to pass to __x_busy */
  int n_busy;                      /* Number of times __x_busy has been invoked */
};
typedef struct superlock_busy_t superlock_busy_t;

/*
** An instance of the following structure is allocated for each active
** superlock. The opaque handle returned by sqlite3demo_superlock() is
** actually a pointer to an instance of this structure.
*/
struct superlock_t {
  DBSQL *db;                    /* Database handle used to lock db */
  int wal;                       /* True if db is a WAL database */
};
typedef struct superlock_t superlock_t;

/*
** The ctx pointer passed to this function is actually a pointer to a
** superlock_busy_t structure. Invoke the busy-handler function encapsulated
** by the structure and return the result.
*/
static int __superlock_busy_handler(void *ctx, int UNUSED){
  superlock_busy_t *busy = (superlock_busy_t *)ctx;
  if( busy->__x_busy==0 ) return 0;
  return busy->__x_busy(busy->busy_arg, busy->n_busy++);
}

/*
** This function is used to determine if the main database file for 
** connection db is open in WAL mode or not. If no error occurs and the
** database file is in WAL mode, set *pb_wal to true and return DBSQL_OK.
** If it is not in WAL mode, set *pb_wal to false.
**
** If an error occurs, return an dbsql error code. The value of *pb_wal
** is undefined in this case.
*/
static int __superlock_is_wal(superlock_t *lock){
  int rc;                         /* Return Code */
  dbsql_stmt_t *stmt;            /* Compiled PRAGMA journal_mode statement */

  rc = dbsql_prepare(lock->db, "PRAGMA main.journal_mode", -1, &stmt, 0);
  if( rc!=DBSQL_OK ) return rc;

  lock->wal = 0;
  if( DBSQL_ROW==dbsql_step(stmt) ){
    const char *mode = (const char *)dbsql_column_text(stmt, 0);
    if( mode && strlen(mode)==3 && dbsql_strnicmp("wal", mode, 3)==0 ){
      lock->wal = 1;
    }
  }

  return dbsql_finalize(stmt);
}

/*
** Obtain an exclusive shm-lock on n_byte bytes starting at offset idx
** of the file fd. If the lock cannot be obtained immediately, invoke
** the busy-handler until either it is obtained or the busy-handler
** callback returns 0.
*/
static int __superlock_shm_lock(
  dbsql_file_t *fd,               /* Database file handle */
  int idx,                        /* Offset of shm-lock to obtain */
  int n_byte,                      /* Number of consective bytes to lock */
  superlock_busy_t *busy            /* Busy-handler wrapper object */
){
  int rc;
  int (*shm_lock)(dbsql_file_t*, int, int, int) = fd->methods->shm_lock;
  do {
    rc = shm_lock(fd, idx, n_byte, DBSQL_SHM_LOCK|DBSQL_SHM_EXCLUSIVE);
  }while( rc==DBSQL_BUSY && __superlock_busy_handler((void *)busy, 0) );
  return rc;
}

/*
** Obtain the extra locks on the database file required for WAL databases.
** Invoke the supplied busy-handler as required.
*/
static int __superlock_wal_lock(
  DBSQL *db,                    /* Database handle open on WAL database */
  superlock_busy_t *busy            /* Busy handler wrapper object */
){
  int rc;                         /* Return code */
  dbsql_file_t *fd = 0;           /* Main database file handle */
  void volatile *p = 0;           /* Pointer to first page of shared memory */

  /* Obtain a pointer to the dbsql_file_t object open on the main db file. */
  rc = dbsql_file_control(db, "main", DBSQL_FCNTL_FILE_POINTER, (void *)&fd);
  if( rc!=DBSQL_OK ) return rc;

  /* Obtain the "recovery" lock. Normally, this lock is only obtained by
  ** clients running database recovery.  
  */
  rc = __superlock_shm_lock(fd, 2, 1, busy);
  if( rc!=DBSQL_OK ) return rc;

  /* Zero the start of the first shared-memory page. This means that any
  ** clients that open read or write transactions from this point on will
  ** have to run recovery before proceeding. Since they need the "recovery"
  ** lock that this process is holding to do that, no new read or write
  ** transactions may now be opened. Nor can a checkpoint be run, for the
  ** same reason.
  */
  rc = fd->methods->shm_map(fd, 0, 32*1024, 1, &p);
  if( rc!=DBSQL_OK ) return rc;
  memset((void *)p, 0, 32);

  /* Obtain exclusive locks on all the "read-lock" slots. Once these locks
  ** are held, it is guaranteed that there are no active reader, writer or 
  ** checkpointer clients.
  */
  rc = __superlock_shm_lock(fd, 3, DBSQL_SHM_NLOCK-3, busy);
  return rc;
}

/*
** Release a superlock held on a database file. The argument passed to 
** this function must have been obtained from a successful call to
** sqlite3demo_superlock().
*/
void __dbsqldemo_superunlock(void *lock){
  superlock_t *p = (superlock_t *)lock;
  if( p->wal ){
    int rc;                         /* Return code */
    int flags = DBSQL_SHM_UNLOCK | DBSQL_SHM_EXCLUSIVE;
    dbsql_file_t *fd = 0;
    rc = dbsql_file_control(p->db, "main", DBSQL_FCNTL_FILE_POINTER, (void *)&fd);
    if( rc==DBSQL_OK ){
      fd->methods->shm_lock(fd, 2, 1, flags);
      fd->methods->shm_lock(fd, 3, DBSQL_SHM_NLOCK-3, flags);
    }
  }
  dbsql_close(p->db);
  dbsql_free(p);
}

/*
** Obtain a superlock on the database file identified by path, using the
** locking primitives provided by VFS vfs. If successful, DBSQL_OK is
** returned and output variable *lock is populated with an opaque handle
** that may be used with sqlite3demo_superunlock() to release the lock.
**
** If an error occurs, *lock is set to 0 and an dbsql error code 
** (e.g. DBSQL_BUSY) is returned.
**
** If a required lock cannot be obtained immediately and the __x_busy parameter
** to this function is not NULL, then __x_busy is invoked in the same way
** as a busy-handler registered with dbsql (using dbsql_busy_handler())
** until either the lock can be obtained or the busy-handler function returns
** 0 (indicating "give up").
*/
int __dbsqldemo_superlock(
  const char *path,              /* Path to database file to lock */
  const char *vfs,               /* VFS to use to access database file */
  int (*__x_busy)(void*,int),        /* Busy handler callback */
  void *busy_arg,                 /* Context arg for busy handler */
  void **pp_lock                   /* OUT: Context to pass to superunlock() */
){
  superlock_busy_t busy = {0, 0, 0}; /* Busy handler wrapper object */
  int rc;                         /* Return code */
  superlock_t *lock;

  lock = dbsql_malloc(sizeof(superlock_t));
  if( !lock ) return DBSQL_NOMEM;
  memset(lock, 0, sizeof(superlock_t));

  /* Open a database handle on the file to superlock. */
  rc = dbsql_open_v2(
      path, &lock->db, DBSQL_OPEN_READWRITE|DBSQL_OPEN_CREATE, vfs
  );

  /* Install a busy-handler and execute a BEGIN EXCLUSIVE. If this is not
  ** a WAL database, this is all we need to do.  
  **
  ** A wrapper function is used to invoke the busy-handler instead of
  ** registering the busy-handler function supplied by the user directly
  ** with dbsql. This is because the same busy-handler function may be
  ** invoked directly later on when attempting to obtain the extra locks
  ** required in WAL mode. By using the wrapper, we are able to guarantee
  ** that the "n_busy" integer parameter passed to the users busy-handler
  ** represents the total number of busy-handler invocations made within
  ** this call to sqlite3demo_superlock(), including any made during the
  ** "BEGIN EXCLUSIVE".
  */
  if( rc==DBSQL_OK ){
    busy.__x_busy = __x_busy;
    busy.busy_arg = busy_arg;
    dbsql_busy_handler(lock->db, __superlock_busy_handler, (void *)&busy);
    rc = dbsql_exec(lock->db, "BEGIN EXCLUSIVE", 0, 0, 0);
  }

  /* If the BEGIN EXCLUSIVE was executed successfully and this is a WAL
  ** database, call __superlock_wal_lock() to obtain the extra locks required
  ** to prevent readers, writers and/or checkpointers from accessing the
  ** db while this process is holding the superlock.
  **
  ** Before attempting any WAL locks, commit the transaction started above
  ** to drop the WAL read and write locks currently held. Otherwise, the
  ** new WAL locks may conflict with the old.
  */
  if( rc==DBSQL_OK ){
    if( DBSQL_OK==(rc = __superlock_is_wal(lock)) && lock->wal ){
      rc = dbsql_exec(lock->db, "COMMIT", 0, 0, 0);
      if( rc==DBSQL_OK ){
        rc = __superlock_wal_lock(lock->db, &busy);
      }
    }
  }

  if( rc!=DBSQL_OK ){
    __dbsqldemo_superunlock(lock);
    *pp_lock = 0;
  }else{
    *pp_lock = lock;
  }

  return rc;
}

/*
** End of example code. Everything below here is the test harness.
**************************************************************************
**************************************************************************
*************************************************************************/


#ifdef DBSQL_TEST

#include "tcl_dbsql.h"

struct interp_and_script_t {
  Tcl_Interp *interp;
  Tcl_Obj *script;
};
typedef struct interp_and_script_t interp_and_script_t;

static void DBSQL_TCLAPI superunlock_del(ClientData cd){
  __dbsqldemo_superunlock((void *)cd);
}

static int DBSQL_TCLAPI superunlock_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
  Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
  return TCL_OK;
}

static int superlock_busy(void *ctx, int n_busy){
  interp_and_script_t *p = (interp_and_script_t *)ctx;
  Tcl_Obj *eval;                 /* Script to evaluate */
  int i_val = 0;                   /* Value to return */

  eval = Tcl_DuplicateObj(p->script);
  Tcl_IncrRefCount(eval);
  Tcl_ListObjAppendElement(p->interp, eval, Tcl_NewIntObj(n_busy));
  Tcl_EvalObjEx(p->interp, eval, TCL_EVAL_GLOBAL);
  Tcl_GetIntFromObj(p->interp, Tcl_GetObjResult(p->interp), &i_val);
  Tcl_DecrRefCount(eval);

  return i_val;
}

/*
** Tclcmd: sqlite3demo_superlock CMDNAME PATH VFS BUSY-HANDLER-SCRIPT
*/
static int DBSQL_TCLAPI superlock_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *lock;                    /* Lock context */
  char *path;
  char *vfs = 0;
  interp_and_script_t busy = {0, 0};
  int (*__x_busy)(void*,int) = 0;    /* Busy handler callback */
  int rc;                         /* Return code from sqlite3demo_superlock() */

  if( objc<3 || objc>5 ){
    Tcl_WrongNumArgs(
        interp, 1, objv, "CMDNAME PATH ?VFS? ?BUSY-HANDLER-SCRIPT?");
    return TCL_ERROR;
  }

  path = Tcl_GetString(objv[2]);

  if( objc>3 ){
    vfs = Tcl_GetString(objv[3]);
    if( strlen(vfs)==0 ) vfs = 0;
  }
  if( objc>4 ){
    busy.interp = interp;
    busy.script = objv[4];
    __x_busy = superlock_busy;
  }

  rc = __dbsqldemo_superlock(path, vfs, __x_busy, &busy, &lock);
  assert( rc==DBSQL_OK || lock==0 );
  assert( rc!=DBSQL_OK || lock!=0 );

  if( rc!=DBSQL_OK ){
    extern const char *__err_str(int);
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, __err_str(rc), NULL);
    return TCL_ERROR;
  }

  Tcl_CreateObjCommand(
      interp, Tcl_GetString(objv[1]), superunlock_cmd, lock, superunlock_del
  );
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

int DbsqlSuperlock_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "dbsql3demo_superlock", superlock_cmd, 0, 0);
  return TCL_OK;
}
#endif
