/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** A TCL Interface to dbsql.  Append this file to DBSQL.c and
** compile the whole thing to build a TCL-enabled version of dbsql.
**
** Compile-time options:
**
**  -DTCLSH         Add a "main()" routine that works as a tclsh.
**
**  -DTCLSH_INIT_PROC=name
**
**                  Invoke name(interp) to initialize the Tcl interpreter.
**                  If name(interp) returns a non-NULL string, then run
**                  that string as a Tcl script to launch the application.
**                  If name(interp) returns NULL, then run the regular
**                  tclsh-emulator code.
*/
#ifdef TCLSH_INIT_PROC
# define TCLSH 1
#endif

/*
** If requested, include the dbsql compiler options file for MSVC.
*/
#if defined(INCLUDE_MSVC_H)
# include "msvc.h"
#endif

/****** Copy of tclsqlite.h ******/
#if defined(INCLUDE_DBSQL_TCL_H)
# include "dbsql_tcl.h"   /* Special case for Windows using STDCALL */
#else
# include <tcl.h>          /* All normal cases */
# ifndef DBSQL_TCLAPI
#   define DBSQL_TCLAPI
# endif
#endif
/* Compatability between Tcl8.6 and Tcl9.0 */
#if TCL_MAJOR_VERSION==9
# define CONST const
#elif !defined(TCL_SIZE)
  typedef int TCL_SIZE;
# ifndef TCL_BOUNCE_REF_COUNT
#  define TCL_BOUNCE_REF_COUNT(X) Tcl_IncrRefCount(X); Tcl_DecrRefCount(X)
   /* https://www.tcl-lang.org/man/tcl9.0/__tcl_lib/Object.html */
# endif
#endif
/**** End copy of tclsqlite.h ****/

#include <errno.h>

/*
** Some additional include files are needed if this file is not
** appended to the amalgamation.
*/
#ifndef DBSQL_AMALGAMATION
# include "dbsql.h"
# include <stdlib.h>
# include <string.h>
# include <assert.h>
  typedef unsigned char u_int8_t;
# ifndef DBSQL_PTRSIZE
#   if defined(__SIZEOF_POINTER__)
#     define DBSQL_PTRSIZE __SIZEOF_POINTER__
#   elif defined(i386)     || defined(__i386__)   || defined(_M_IX86) ||    \
         defined(_M_ARM)   || defined(__arm__)    || defined(__x86)   ||    \
        (defined(__APPLE__) && defined(__POWERPC__)) ||                     \
        (defined(__TOS_AIX__) && !defined(__64BIT__))
#     define DBSQL_PTRSIZE 4
#   else
#     define DBSQL_PTRSIZE 8
#   endif
# endif /* DBSQL_PTRSIZE */
# if defined(HAVE_STDINT_H) || (defined(__STDC_VERSION__) &&  \
                                (__STDC_VERSION__ >= 199901L))
#   include <stdint.h>
    typedef uintptr_t uptr_t;
# elif DBSQL_PTRSIZE==4
    typedef unsigned int uptr_t;
# else
    typedef u_int64_t uptr_t;
# endif
#endif
#include <ctype.h>

/* Used to get the current process ID */
#if !defined(_WIN32)
# include <signal.h>
# include <unistd.h>
# define GETPID getpid
#elif !defined(_WIN32_WCE)
# ifndef DBSQL_AMALGAMATION
#  ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
# endif
# include <io.h>
# define isatty(h) _isatty(h)
# define GETPID (int)__get_current_process_id
#endif

/*
 * Windows needs to know which symbols to export.  Unix does not.
 * BUILD_sqlite should be undefined for Unix.
 */
#ifdef BUILD_dbsql
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT
#endif /* BUILD_sqlite */

#define NUM_PREPARED_STMTS 10
#define MAX_PREPARED_STMTS 100

/* Forward declaration */
typedef struct dbsql_db dbsql_db;

/* Add -DSQLITE_ENABLE_QRF_IN_TCL to add the Query Result Formatter (QRF)
** into the build of the TCL extension, when building using separate
** source files.  The QRF is included automatically when building from
** the tclsqlite3.c amalgamation.
*/
#if defined(DBSQL_ENABLE_QRF_IN_TCL)
#include "qrf.h"
#endif

/*
** New SQL functions can be created as TCL scripts.  Each such function
** is described by an instance of the following structure.
**
** Variable type may be set to DBSQL_INTEGER, DBSQL_FLOAT, DBSQL_TEXT,
** DBSQL_BLOB or DBSQL_NULL. If it is DBSQL_NULL, then the implementation
** attempts to determine the type of the result based on the Tcl object.
** If it is DBSQL_TEXT or DBSQL_BLOB, then a text (dbsql_result_text())
** or blob (dbsql_result_blob()) is returned. If it is DBSQL_INTEGER
** or DBSQL_FLOAT, then an attempt is made to return an integer or float
** value, falling back to float and then text if this is not possible.
*/
typedef struct sql_func_t sql_func_t;
struct sql_func_t {
  Tcl_Interp *interp;   /* The TCL interpret to execute the function */
  Tcl_Obj *script;     /* The Tcl_Obj representation of the script */
  dbsql_db *db;        /* Database connection that owns this function */
  int use_eval_objv;      /* True if it is safe to use Tcl_EvalObjv */
  int type;            /* Type of value to return */
  char *name;          /* Name of this function */
  sql_func_t *next;       /* Next function on the list of them all */
};

/*
** New collation sequences function can be created as TCL scripts.  Each such
** function is described by an instance of the following structure.
*/
typedef struct sql_collate_t sql_collate_t;
struct sql_collate_t {
  Tcl_Interp *interp;   /* The TCL interpret to execute the function */
  char *script;        /* The script to be run */
  sql_collate_t *next;    /* Next function on the list of them all */
};

/*
** Prepared statements are cached for faster execution.  Each prepared
** statement is described by an instance of the following structure.
*/
typedef struct sql_prepared_stmt_t sql_prepared_stmt_t;
struct sql_prepared_stmt_t {
  sql_prepared_stmt_t *next;  /* Next in linked list */
  sql_prepared_stmt_t *prev;  /* Previous on the list */
  dbsql_stmt_t *stmt;     /* The prepared statement */
  int n_sql;                /* chars in sql[] */
  const char *z_sql;        /* Text of the SQL statement */
  int n_parm;               /* Size of parms array */
  Tcl_Obj **parms;        /* Array of referenced object pointers */
};

typedef struct incrblob_channel_t incrblob_channel_t;

/*
** There is one instance of this structure for each dbsql database
** that has been opened by the dbsql TCL interface.
**
** If this module is built with DBSQL_TEST defined (to create the dbsql
** testfixture executable), then it may be configured to use either
** dbsql_prepare_v2() or dbsql_prepare() to prepare SQL statements.
** If dbsql_db.legacy_prepare is true, dbsql_prepare() is used.
*/
struct dbsql_db {
  DBSQL *db;               /* The "real" database structure. MUST BE FIRST */
  Tcl_Interp *interp;        /* The interpreter used for this database */
  char *busy;               /* The busy callback routine */
  char *commit;             /* The commit hook callback routine */
  char *trace;              /* The trace callback routine */
  char *trace_v2;            /* The trace_v2 callback routine */
  char *profile;            /* The profile callback routine */
  char *progress;           /* The progress callback routine */
  char *bind_fallback;       /* Callback to invoke on a binding miss */
  char *auth;               /* The authorization callback routine */
  int disable_auth;           /* Disable the authorizer if it exists */
  char *null;               /* Text to substitute for an SQL NULL value */
  sql_func_t *func;            /* List of SQL functions */
  Tcl_Obj *update_hook;      /* Update hook script (if any) */
  Tcl_Obj *pre_update_hook;   /* Pre-update hook script (if any) */
  Tcl_Obj *rollback_hook;    /* Rollback hook script (if any) */
  Tcl_Obj *wal_hook;         /* WAL hook script (if any) */
  Tcl_Obj *unlock_notify;    /* Unlock notify script (if any) */
  sql_collate_t *collate;      /* List of SQL collation functions */
  int rc;                    /* Return code of most recent dbsql_exec() */
  Tcl_Obj *collate_needed;   /* Collation needed script */
  sql_prepared_stmt_t *stmt_list; /* List of prepared statements*/
  sql_prepared_stmt_t *stmt_last; /* Last statement in the list */
  int max_stmt;               /* The next maximum number of stmt_list */
  int n_stmt;                 /* Number of statements in stmt_list */
  incrblob_channel_t *incrblob;/* Linked list of open incrblob channels */
  int n_step, n_sort, n_index;  /* Statistics for most recent operation */
  int n_vm_step;               /* Another statistic for most recent operation */
  int n_transaction;          /* Number of nested [transaction] methods */
  int open_flags;             /* Flags used to open.  (DBSQL_OPEN_URI) */
  int n_ref;                  /* Delete object when this reaches 0 */
#ifdef DBSQL_TEST
  int legacy_prepare;        /* True to use dbsql_prepare() */
#endif
};

struct incrblob_channel_t {
  dbsql_blob_t *blob;      /* DBSQL blob handle */
  dbsql_db *db;            /* Associated database connection */
  int64_t i_seek;      /* Current seek offset */
  unsigned int is_closed;    /* TCL_CLOSE_READ or TCL_CLOSE_WRITE */
  Tcl_Channel channel;      /* Channel identifier */
  incrblob_channel_t *next;   /* Linked list of all open incrblob channels */
  incrblob_channel_t *prev;   /* Linked list of all open incrblob channels */
};

/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
*/
static int strlen30(const char *z){
  const char *z2 = z;
  while( *z2 ){ z2++; }
  return 0x3fffffff & (int)(z2 - z);
}


#ifndef DBSQL_OMIT_INCRBLOB
/*
** Close all incrblob channels opened using database connection db.
** This is called when shutting down the database connection.
*/
static void CLOSE_INCRBLOB_CHANNELS(dbsql_db *db){
  incrblob_channel_t *p;
  incrblob_channel_t *next;

  for(p=db->incrblob; p; p=next){
    next = p->next;

    /* Note: Calling unregister here call Tcl_Close on the incrblob channel,
    ** which deletes the incrblob_channel_t structure at *p. So do not
    ** call Tcl_Free() here.
    */
    Tcl_UnregisterChannel(db->interp, p->channel);
  }
}

/*
** Close an incremental blob channel.
*/
static int DBSQL_TCLAPI __incrblob_close2(
  ClientData instanceData,
  Tcl_Interp *interp,
  int flags
){
  incrblob_channel_t *p = (incrblob_channel_t *)instanceData;
  int  rc;
  DBSQL *db = p->db->db;

  if( flags ){
    p->is_closed |= flags;
    return TCL_OK;
  }

  /* If we reach this point, then we really do need to close the channel */
  rc = dbsql_blob_close(p->blob);

  /* Remove the channel from the dbsql_db.incrblob list. */
  if( p->next ){
    p->next->prev = p->prev;
  }
  if( p->prev ){
    p->prev->next = p->next;
  }
  if( p->db->incrblob==p ){
    p->db->incrblob = p->next;
  }

  /* Free the incrblob_channel_t structure */
  Tcl_Free((char *)p);

  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char *)dbsql_errmsg(db), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}
static int DBSQL_TCLAPI __incrblob_close(
  ClientData instanceData,
  Tcl_Interp *interp
){
  return __incrblob_close2(instanceData, interp, 0);
}


/*
** Read data from an incremental blob channel.
*/
static int DBSQL_TCLAPI __incrblob_input(
  ClientData instanceData,
  char *buf,
  int buf_size,
  int *errorCodePtr
){
  incrblob_channel_t *p = (incrblob_channel_t *)instanceData;
  int64_t n_read = buf_size;   /* Number of bytes to read */
  int64_t n_blob;             /* Total size of the blob */
  int rc;                          /* sqlite error code */

  n_blob = dbsql_blob_bytes(p->blob);
  if( (p->i_seek+n_read)>n_blob ){
    n_read = n_blob-p->i_seek;
  }
  if( n_read<=0 ){
    return 0;
  }

  rc = dbsql_blob_read(p->blob, (void *)buf, (int)n_read, (int)p->i_seek);
  if( rc!=DBSQL_OK ){
    *errorCodePtr = rc;
    return -1;
  }

  p->i_seek += n_read;
  return n_read;
}

/*
** Write data to an incremental blob channel.
*/
static int DBSQL_TCLAPI __incrblob_output(
  ClientData instanceData,
  const char *buf,
  int toWrite,
  int *errorCodePtr
){
  incrblob_channel_t *p = (incrblob_channel_t *)instanceData;
  int64_t n_write = toWrite;   /* Number of bytes to write */
  int64_t n_blob;              /* Total size of the blob */
  int rc;                           /* sqlite error code */

  n_blob = dbsql_blob_bytes(p->blob);
  if( (p->i_seek+n_write)>n_blob ){
    *errorCodePtr = EINVAL;
    return -1;
  }
  if( n_write<=0 ){
    return 0;
  }

  rc = dbsql_blob_write(p->blob, (void*)buf,(int)n_write, (int)p->i_seek);
  if( rc!=DBSQL_OK ){
    *errorCodePtr = EIO;
    return -1;
  }

  p->i_seek += n_write;
  return n_write;
}

/* The datatype of Tcl_DriverWideSeekProc changes between tcl8.6 and tcl9.0 */
#if TCL_MAJOR_VERSION==9
# define WIDE_SEEK_PROC_TYPE long long
#else
# define WIDE_SEEK_PROC_TYPE Tcl_WideInt
#endif

/*
** Seek an incremental blob channel.
*/
static WIDE_SEEK_PROC_TYPE DBSQL_TCLAPI __incrblob_wide_seek(
  ClientData instanceData,
  WIDE_SEEK_PROC_TYPE offset,
  int seek_mode,
  int *errorCodePtr
){
  incrblob_channel_t *p = (incrblob_channel_t *)instanceData;

  switch( seek_mode ){
    case SEEK_SET:
      p->i_seek = offset;
      break;
    case SEEK_CUR:
      p->i_seek += offset;
      break;
    case SEEK_END:
      p->i_seek = dbsql_blob_bytes(p->blob) + offset;
      break;

    default: assert(!"Bad seekMode");
  }

  return p->i_seek;
}
static int DBSQL_TCLAPI __incrblob_seek(
  ClientData instanceData,
  long offset,
  int seek_mode,
  int *errorCodePtr
){
  return __incrblob_wide_seek(instanceData,offset,seek_mode,errorCodePtr);
}


static void DBSQL_TCLAPI __incrblob_watch(
  ClientData instanceData,
  int mode
){
  /* NO-OP */
}
static int DBSQL_TCLAPI __incrblob_handle(
  ClientData instanceData,
  int dir,
  ClientData *hPtr
){
  return TCL_ERROR;
}

static Tcl_ChannelType __incrblob_channel_type = {
  "incrblob",                        /* typeName                             */
  TCL_CHANNEL_VERSION_5,             /* version                              */
  __incrblob_close,                     /* closeProc                            */
  __incrblob_input,                     /* inputProc                            */
  __incrblob_output,                    /* outputProc                           */
  __incrblob_seek,                      /* seekProc                             */
  0,                                 /* setOptionProc                        */
  0,                                 /* getOptionProc                        */
  __incrblob_watch,                     /* watchProc (this is a no-op)          */
  __incrblob_handle,                    /* getHandleProc (always returns error) */
  __incrblob_close2,                    /* close2Proc                           */
  0,                                 /* blockModeProc                        */
  0,                                 /* flushProc                            */
  0,                                 /* handlerProc                          */
  __incrblob_wide_seek,                  /* wideSeekProc                         */
};

/*
** Create a new incrblob channel.
*/
static int __create_incrblob_channel(
  Tcl_Interp *interp,
  dbsql_db *p_db,
  const char *z_db,
  const char *table,
  const char *column,
  int64_t i_row,
  int is_readonly
){
  incrblob_channel_t *p;
  DBSQL *db = p_db->db;
  dbsql_blob_t *blob;
  int rc;
  int flags = TCL_READABLE|(is_readonly ? 0 : TCL_WRITABLE);

  /* This variable is used to name the channels: "incrblob_[incr count]" */
  static int count = 0;
  char channel[64];

  rc = dbsql_blob_open(db, z_db, table, column, i_row, !is_readonly, &blob);
  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char *)dbsql_errmsg(p_db->db), TCL_VOLATILE);
    return TCL_ERROR;
  }

  p = (incrblob_channel_t *)Tcl_Alloc(sizeof(incrblob_channel_t));
  memset(p, 0, sizeof(*p));
  p->blob = blob;
  if( (flags & TCL_WRITABLE)==0 ) p->is_closed |= TCL_CLOSE_WRITE;

  dbsql_snprintf(sizeof(channel), channel, "incrblob_%d", ++count);
  p->channel = Tcl_CreateChannel(&__incrblob_channel_type, channel, p, flags);
  Tcl_RegisterChannel(interp, p->channel);

  /* Link the new channel into the dbsql_db.incrblob list. */
  p->next = p_db->incrblob;
  p->prev = 0;
  if( p->next ){
    p->next->prev = p;
  }
  p_db->incrblob = p;
  p->db = p_db;

  Tcl_SetResult(interp, (char *)Tcl_GetChannelName(p->channel), TCL_VOLATILE);
  return TCL_OK;
}
#else  /* else clause for "#ifndef DBSQL_OMIT_INCRBLOB" */
  #define CLOSE_INCRBLOB_CHANNELS(db)
#endif

/*
** Look at the script prefix in cmd.  We will be executing this script
** after first appending one or more arguments.  This routine analyzes
** the script to see if it is safe to use Tcl_EvalObjv() on the script
** rather than the more general Tcl_EvalEx().  Tcl_EvalObjv() is much
** faster.
**
** Scripts that are safe to use with Tcl_EvalObjv() consists of a
** command name followed by zero or more arguments with no [...] or $
** or {...} or ; to be seen anywhere.  Most callback scripts consist
** of just a single procedure name and they meet this requirement.
*/
static int __safe_to_use_eval_objv(Tcl_Obj *cmd){
  /* We could try to do something with Tcl_Parse().  But we will instead
  ** just do a search for forbidden characters.  If any of the forbidden
  ** characters appear in cmd, we will report the string as unsafe.
  */
  const char *z;
  TCL_SIZE n;
  z = Tcl_GetStringFromObj(cmd, &n);
  while( n-- > 0 ){
    int c = *(z++);
    if( c=='$' || c=='[' || c==';' ) return 0;
  }
  return 1;
}

/*
** Find an sql_func_t structure with the given name.  Or create a new
** one if an existing one cannot be found.  Return a pointer to the
** structure.
*/
static sql_func_t *__find_sql_func(dbsql_db *db, const char *name){
  sql_func_t *p, *p_new;
  int n_name = strlen30(name);
  p_new = (sql_func_t*)Tcl_Alloc( sizeof(*p_new) + n_name + 1 );
  p_new->name = (char*)&p_new[1];
  memcpy(p_new->name, name, n_name+1);
  for(p=db->func; p; p=p->next){
    if( dbsql_stricmp(p->name, p_new->name)==0 ){
      Tcl_Free((char*)p_new);
      return p;
    }
  }
  p_new->interp = db->interp;
  p_new->db = db;
  p_new->script = 0;
  p_new->next = db->func;
  db->func = p_new;
  return p_new;
}

/*
** Free a single sql_prepared_stmt_t object.
*/
static void __db_free_stmt(sql_prepared_stmt_t *stmt){
#ifdef DBSQL_TEST
  if( dbsql_sql(stmt->stmt)==0 ){
    Tcl_Free((char *)stmt->z_sql);
  }
#endif
  dbsql_finalize(stmt->stmt);
  Tcl_Free((char *)stmt);
}

/*
** Finalize and free a list of prepared statements
*/
static void __flush_stmt_cache(dbsql_db *db){
  sql_prepared_stmt_t *pre_stmt;
  sql_prepared_stmt_t *next;

  for(pre_stmt = db->stmt_list; pre_stmt; pre_stmt=next){
    next = pre_stmt->next;
    __db_free_stmt(pre_stmt);
  }
  db->n_stmt = 0;
  db->stmt_last = 0;
  db->stmt_list = 0;
}

/*
** Increment the reference counter on the dbsql_db object. The reference
** should be released by calling __del_database_ref().
*/
static void __add_database_ref(dbsql_db *db){
  db->n_ref++;
}

/*
** Decrement the reference counter associated with the dbsql_db object.
** If it reaches zero, delete the object.
*/
static void __del_database_ref(dbsql_db *db){
  assert( db->n_ref>0 );
  db->n_ref--;
  if( db->n_ref==0 ){
    __flush_stmt_cache(db);
    CLOSE_INCRBLOB_CHANNELS(db);
    dbsql_close(db->db);
    while( db->func ){
      sql_func_t *func = db->func;
      db->func = func->next;
      assert( func->db==db );
      Tcl_DecrRefCount(func->script);
      Tcl_Free((char*)func);
    }
    while( db->collate ){
      sql_collate_t *collate = db->collate;
      db->collate = collate->next;
      Tcl_Free((char*)collate);
    }
    if( db->busy ){
      Tcl_Free(db->busy);
    }
    if( db->trace ){
      Tcl_Free(db->trace);
    }
    if( db->trace_v2 ){
      Tcl_Free(db->trace_v2);
    }
    if( db->profile ){
      Tcl_Free(db->profile);
    }
    if( db->bind_fallback ){
      Tcl_Free(db->bind_fallback);
    }
    if( db->auth ){
      Tcl_Free(db->auth);
    }
    if( db->null ){
      Tcl_Free(db->null);
    }
    if( db->update_hook ){
      Tcl_DecrRefCount(db->update_hook);
    }
    if( db->pre_update_hook ){
      Tcl_DecrRefCount(db->pre_update_hook);
    }
    if( db->rollback_hook ){
      Tcl_DecrRefCount(db->rollback_hook);
    }
    if( db->wal_hook ){
      Tcl_DecrRefCount(db->wal_hook);
    }
    if( db->collate_needed ){
      Tcl_DecrRefCount(db->collate_needed);
    }
    Tcl_Free((char*)db);
  }
}

/*
** TCL calls this procedure when an DBSQL database command is
** deleted.
*/
static void DBSQL_TCLAPI __db_delete_cmd(void *db){
  dbsql_db *p_db = (dbsql_db*)db;
  __del_database_ref(p_db);
}

/*
** This routine is called when a database file is locked while trying
** to execute SQL.
*/
static int __db_busy_handler(void *cd, int n_tries){
  dbsql_db *db = (dbsql_db*)cd;
  int rc;
  char val[30];

  dbsql_snprintf(sizeof(val), val, "%d", n_tries);
  rc = Tcl_VarEval(db->interp, db->busy, " ", val, (char*)0);
  if( rc!=TCL_OK || atoi(Tcl_GetStringResult(db->interp)) ){
    return 0;
  }
  return 1;
}

#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
/*
** This routine is invoked as the 'progress callback' for the database.
*/
static int __db_progress_handler(void *cd){
  dbsql_db *db = (dbsql_db*)cd;
  int rc;

  assert( db->progress );
  rc = Tcl_Eval(db->interp, db->progress);
  if( rc!=TCL_OK || atoi(Tcl_GetStringResult(db->interp)) ){
    return 1;
  }
  return 0;
}
#endif

#if !defined(DBSQL_OMIT_TRACE) && !defined(DBSQL_OMIT_FLOATING_POINT) && \
    !defined(DBSQL_OMIT_DEPRECATED)
/*
** This routine is called by the dbsql trace handler whenever a new
** block of SQL is executed.  The TCL script in db->trace is executed.
*/
static void __db_trace_handler(void *cd, const char *sql){
  dbsql_db *db = (dbsql_db*)cd;
  Tcl_DString str;

  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, db->trace, -1);
  Tcl_DStringAppendElement(&str, sql);
  Tcl_Eval(db->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  Tcl_ResetResult(db->interp);
}
#endif

#ifndef DBSQL_OMIT_TRACE
/*
** This routine is called by the dbsql trace_v2 handler whenever a new
** supported event is generated.  Unsupported event types are ignored.
** The TCL script in db->trace_v2 is executed, with the arguments for
** the event appended to it (as list elements).
*/
static int __db_trace_v2_handler(
  unsigned type, /* One of the DBSQL_TRACE_* event types. */
  void *cd,      /* The original context data pointer. */
  void *pd,      /* Primary event data, depends on event type. */
  void *xd       /* Extra event data, depends on event type. */
){
  dbsql_db *p_db = (dbsql_db*)cd;
  Tcl_Obj *cmd;

  switch( type ){
    case DBSQL_TRACE_STMT: {
      dbsql_stmt_t *stmt = (dbsql_stmt_t *)pd;
      char *sql = (char *)xd;

      cmd = Tcl_NewStringObj(p_db->trace_v2, -1);
      Tcl_IncrRefCount(cmd);
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewWideIntObj((Tcl_WideInt)(uptr_t)stmt));
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewStringObj(sql, -1));
      Tcl_EvalObjEx(p_db->interp, cmd, TCL_EVAL_DIRECT);
      Tcl_DecrRefCount(cmd);
      Tcl_ResetResult(p_db->interp);
      break;
    }
    case DBSQL_TRACE_PROFILE: {
      dbsql_stmt_t *stmt = (dbsql_stmt_t *)pd;
      int64_t ns = *(int64_t*)xd;

      cmd = Tcl_NewStringObj(p_db->trace_v2, -1);
      Tcl_IncrRefCount(cmd);
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewWideIntObj((Tcl_WideInt)(uptr_t)stmt));
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewWideIntObj((Tcl_WideInt)ns));
      Tcl_EvalObjEx(p_db->interp, cmd, TCL_EVAL_DIRECT);
      Tcl_DecrRefCount(cmd);
      Tcl_ResetResult(p_db->interp);
      break;
    }
    case DBSQL_TRACE_ROW: {
      dbsql_stmt_t *stmt = (dbsql_stmt_t *)pd;

      cmd = Tcl_NewStringObj(p_db->trace_v2, -1);
      Tcl_IncrRefCount(cmd);
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewWideIntObj((Tcl_WideInt)(uptr_t)stmt));
      Tcl_EvalObjEx(p_db->interp, cmd, TCL_EVAL_DIRECT);
      Tcl_DecrRefCount(cmd);
      Tcl_ResetResult(p_db->interp);
      break;
    }
    case DBSQL_TRACE_CLOSE: {
      DBSQL *db = (DBSQL *)pd;

      cmd = Tcl_NewStringObj(p_db->trace_v2, -1);
      Tcl_IncrRefCount(cmd);
      Tcl_ListObjAppendElement(p_db->interp, cmd,
                               Tcl_NewWideIntObj((Tcl_WideInt)(uptr_t)db));
      Tcl_EvalObjEx(p_db->interp, cmd, TCL_EVAL_DIRECT);
      Tcl_DecrRefCount(cmd);
      Tcl_ResetResult(p_db->interp);
      break;
    }
  }
  return DBSQL_OK;
}
#endif

#if !defined(DBSQL_OMIT_TRACE) && !defined(DBSQL_OMIT_FLOATING_POINT) && \
    !defined(DBSQL_OMIT_DEPRECATED)
/*
** This routine is called by the dbsql profile handler after a statement
** SQL has executed.  The TCL script in db->profile is evaluated.
*/
static void __db_profile_handler(void *cd, const char *sql, u_int64_t tm){
  dbsql_db *db = (dbsql_db*)cd;
  Tcl_DString str;
  char z_tm[100];

  dbsql_snprintf(sizeof(z_tm)-1, z_tm, "%lld", tm);
  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, db->profile, -1);
  Tcl_DStringAppendElement(&str, sql);
  Tcl_DStringAppendElement(&str, z_tm);
  Tcl_Eval(db->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  Tcl_ResetResult(db->interp);
}
#endif

/*
** This routine is called when a transaction is committed.  The
** TCL script in db->commit is executed.  If it returns non-zero or
** if it throws an exception, the transaction is rolled back instead
** of being committed.
*/
static int __db_commit_handler(void *cd){
  dbsql_db *db = (dbsql_db*)cd;
  int rc;

  rc = Tcl_Eval(db->interp, db->commit);
  if( rc!=TCL_OK || atoi(Tcl_GetStringResult(db->interp)) ){
    return 1;
  }
  return 0;
}

static void __db_rollback_handler(void *clientData){
  dbsql_db *db = (dbsql_db*)clientData;
  assert(db->rollback_hook);
  if( TCL_OK!=Tcl_EvalObjEx(db->interp, db->rollback_hook, 0) ){
    Tcl_BackgroundError(db->interp);
  }
}

/*
** This procedure handles wal_hook callbacks.
*/
static int __db_wal_handler(
  void *clientData,
  DBSQL *db,
  const char *z_db,
  int n_entry
){
  int ret = DBSQL_OK;
  Tcl_Obj *p;
  dbsql_db *p_db = (dbsql_db*)clientData;
  Tcl_Interp *interp = p_db->interp;
  assert(p_db->wal_hook);

  assert( db==p_db->db );
  p = Tcl_DuplicateObj(p_db->wal_hook);
  Tcl_IncrRefCount(p);
  Tcl_ListObjAppendElement(interp, p, Tcl_NewStringObj(z_db, -1));
  Tcl_ListObjAppendElement(interp, p, Tcl_NewIntObj(n_entry));
  if( TCL_OK!=Tcl_EvalObjEx(interp, p, 0)
   || TCL_OK!=Tcl_GetIntFromObj(interp, Tcl_GetObjResult(interp), &ret)
  ){
    Tcl_BackgroundError(interp);
  }
  Tcl_DecrRefCount(p);

  return ret;
}

#if defined(DBSQL_TEST) && defined(DBSQL_ENABLE_UNLOCK_NOTIFY)
static void SET_TEST_UNLOCK_NOTIFY_VARS(Tcl_Interp *interp, int i_arg, int n_arg){
  char buf[64];
  dbsql_snprintf(sizeof(buf), buf, "%d", i_arg);
  Tcl_SetVar(interp, "dbsql_unlock_notify_arg", buf, TCL_GLOBAL_ONLY);
  dbsql_snprintf(sizeof(buf), buf, "%d", n_arg);
  Tcl_SetVar(interp, "dbsql_unlock_notify_argcount", buf, TCL_GLOBAL_ONLY);
}
#else
# define SET_TEST_UNLOCK_NOTIFY_VARS(x,y,z)
#endif

#ifdef DBSQL_ENABLE_UNLOCK_NOTIFY
static void __db_unlock_notify(void **args, int n_arg){
  int i;
  for(i=0; i<n_arg; i++){
    const int flags = (TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT);
    dbsql_db *db = (dbsql_db *)args[i];
    SET_TEST_UNLOCK_NOTIFY_VARS(db->interp, i, n_arg);
    assert( db->unlock_notify);
    Tcl_EvalObjEx(db->interp, db->unlock_notify, flags);
    Tcl_DecrRefCount(db->unlock_notify);
    db->unlock_notify = 0;
  }
}
#endif

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** Pre-update hook callback.
*/
static void __db_pre_update_handler(
  void *p,
  DBSQL *db,
  int op,
  const char *z_db,
  const char *tbl,
  int64_t i_key1,
  int64_t i_key2
){
  dbsql_db *p_db = (dbsql_db *)p;
  Tcl_Obj *cmd;
  static const char *strs[] = {"DELETE", "INSERT", "UPDATE"};

  assert( (DBSQL_DELETE-1)/9 == 0 );
  assert( (DBSQL_INSERT-1)/9 == 1 );
  assert( (DBSQL_UPDATE-1)/9 == 2 );
  assert( p_db->pre_update_hook );
  assert( db==p_db->db );
  assert( op==DBSQL_INSERT || op==DBSQL_UPDATE || op==DBSQL_DELETE );

  cmd = Tcl_DuplicateObj(p_db->pre_update_hook);
  Tcl_IncrRefCount(cmd);
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(strs[(op-1)/9], -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(z_db, -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(tbl, -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewWideIntObj(i_key1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewWideIntObj(i_key2));
  Tcl_EvalObjEx(p_db->interp, cmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(cmd);
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

static void __db_update_handler(
  void *p,
  int op,
  const char *z_db,
  const char *tbl,
  int64_t rowid
){
  dbsql_db *db = (dbsql_db *)p;
  Tcl_Obj *cmd;
  static const char *strs[] = {"DELETE", "INSERT", "UPDATE"};

  assert( (DBSQL_DELETE-1)/9 == 0 );
  assert( (DBSQL_INSERT-1)/9 == 1 );
  assert( (DBSQL_UPDATE-1)/9 == 2 );

  assert( db->update_hook );
  assert( op==DBSQL_INSERT || op==DBSQL_UPDATE || op==DBSQL_DELETE );

  cmd = Tcl_DuplicateObj(db->update_hook);
  Tcl_IncrRefCount(cmd);
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(strs[(op-1)/9], -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(z_db, -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewStringObj(tbl, -1));
  Tcl_ListObjAppendElement(0, cmd, Tcl_NewWideIntObj(rowid));
  Tcl_EvalObjEx(db->interp, cmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(cmd);
}

static void __tcl_collate_needed(
  void *ctx,
  DBSQL *db,
  int enc,
  const char *name
){
  dbsql_db *p_db = (dbsql_db *)ctx;
  Tcl_Obj *script = Tcl_DuplicateObj(p_db->collate_needed);
  Tcl_IncrRefCount(script);
  Tcl_ListObjAppendElement(0, script, Tcl_NewStringObj(name, -1));
  Tcl_EvalObjEx(p_db->interp, script, 0);
  Tcl_DecrRefCount(script);
}

/*
** This routine is called to evaluate an SQL collation function implemented
** using TCL script.
*/
static int __tcl_sql_collate(
  void *ctx,
  int n_a,
  const void *a,
  int n_b,
  const void *b
){
  sql_collate_t *p = (sql_collate_t *)ctx;
  Tcl_Obj *cmd;

  cmd = Tcl_NewStringObj(p->script, -1);
  Tcl_IncrRefCount(cmd);
  Tcl_ListObjAppendElement(p->interp, cmd, Tcl_NewStringObj(a, n_a));
  Tcl_ListObjAppendElement(p->interp, cmd, Tcl_NewStringObj(b, n_b));
  Tcl_EvalObjEx(p->interp, cmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(cmd);
  return (atoi(Tcl_GetStringResult(p->interp)));
}

/*
** This routine is called to evaluate an SQL function implemented
** using TCL script.
*/
static void __tcl_sql_func(dbsql_func_t *context, int argc, dbsql_value_t**argv){
  sql_func_t *p = dbsql_user_data(context);
  Tcl_Obj *cmd;
  int i;
  int rc;

  if( argc==0 ){
    /* If there are no arguments to the function, call Tcl_EvalObjEx on the
    ** script object directly.  This allows the TCL compiler to generate
    ** bytecode for the command on the first invocation and thus make
    ** subsequent invocations much faster. */
    cmd = p->script;
    Tcl_IncrRefCount(cmd);
    rc = Tcl_EvalObjEx(p->interp, cmd, 0);
    Tcl_DecrRefCount(cmd);
  }else{
    /* If there are arguments to the function, make a shallow copy of the
    ** script object, lappend the arguments, then evaluate the copy.
    **
    ** By "shallow" copy, we mean only the outer list Tcl_Obj is duplicated.
    ** The new Tcl_Obj contains pointers to the original list elements.
    ** That way, when Tcl_EvalObjv() is run and shimmers the first element
    ** of the list to tcl_cmd_name_type, that alternate representation will
    ** be preserved and reused on the next invocation.
    */
    Tcl_Obj **args;
    TCL_SIZE n_arg;
    if( Tcl_ListObjGetElements(p->interp, p->script, &n_arg, &args) ){
      dbsql_result_error(context, Tcl_GetStringResult(p->interp), -1);
      return;
    }
    cmd = Tcl_NewListObj(n_arg, args);
    Tcl_IncrRefCount(cmd);
    for(i=0; i<argc; i++){
      dbsql_value_t *in = argv[i];
      Tcl_Obj *val;

      /* Set val to contain the i'th column of this row. */
      switch( dbsql_value_type(in) ){
        case DBSQL_BLOB: {
          int bytes = dbsql_value_bytes(in);
          val = Tcl_NewByteArrayObj(dbsql_value_blob(in), bytes);
          break;
        }
        case DBSQL_INTEGER: {
          int64_t v = dbsql_value_int64(in);
          if( v>=-2147483647 && v<=2147483647 ){
            val = Tcl_NewIntObj((int)v);
          }else{
            val = Tcl_NewWideIntObj(v);
          }
          break;
        }
        case DBSQL_FLOAT: {
          double r = dbsql_value_double(in);
          val = Tcl_NewDoubleObj(r);
          break;
        }
        case DBSQL_NULL: {
          val = Tcl_NewStringObj(p->db->null, -1);
          break;
        }
        default: {
          int bytes = dbsql_value_bytes(in);
          val = Tcl_NewStringObj((char *)dbsql_value_text(in), bytes);
          break;
        }
      }
      rc = Tcl_ListObjAppendElement(p->interp, cmd, val);
      if( rc ){
        Tcl_DecrRefCount(cmd);
        dbsql_result_error(context, Tcl_GetStringResult(p->interp), -1);
        return;
      }
    }
    if( !p->use_eval_objv ){
      /* Tcl_EvalObjEx() will automatically call Tcl_EvalObjv() if cmd
      ** is a list without a string representation.  To prevent this from
      ** happening, make sure cmd has a valid string representation */
      Tcl_GetString(cmd);
    }
    rc = Tcl_EvalObjEx(p->interp, cmd, TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(cmd);
  }

  if( TCL_BREAK==rc ){
    dbsql_result_null(context);
  }else if( rc && rc!=TCL_RETURN ){
    dbsql_result_error(context, Tcl_GetStringResult(p->interp), -1);
  }else{
    Tcl_Obj *var = Tcl_GetObjResult(p->interp);
    TCL_SIZE n;
    u_int8_t *data;
    const char *z_type = (var->typePtr ? var->typePtr->name : "");
    char c = z_type[0];
    int type = p->type;

    if( type==DBSQL_NULL ){
      if( c=='b' && strcmp(z_type,"bytearray")==0 && var->bytes==0 ){
        /* Only return a BLOB type if the Tcl variable is a bytearray and
        ** has no string representation. */
        type = DBSQL_BLOB;
      }else if( (c=='b' && var->bytes==0 && strcmp(z_type,"boolean")==0 )
             || (c=='b' && var->bytes==0 && strcmp(z_type,"booleanString")==0 )
             || (c=='w' && strcmp(z_type,"wideInt")==0)
             || (c=='i' && strcmp(z_type,"int")==0)
      ){
        type = DBSQL_INTEGER;
      }else if( c=='d' && strcmp(z_type,"double")==0 ){
        type = DBSQL_FLOAT;
      }else{
        type = DBSQL_TEXT;
      }
    }

    switch( type ){
      case DBSQL_BLOB: {
        data = Tcl_GetByteArrayFromObj(var, &n);
        dbsql_result_blob(context, data, n, DBSQL_TRANSIENT);
        break;
      }
      case DBSQL_INTEGER: {
        Tcl_WideInt v;
        if( TCL_OK==Tcl_GetWideIntFromObj(0, var, &v) ){
          dbsql_result_int64(context, v);
          break;
        }
        /* fall-through */
      }
      case DBSQL_FLOAT: {
        double r;
        if( TCL_OK==Tcl_GetDoubleFromObj(0, var, &r) ){
          dbsql_result_double(context, r);
          break;
        }
        /* fall-through */
      }
      default: {
        data = (unsigned char *)Tcl_GetStringFromObj(var, &n);
        dbsql_result_text64(context, (char *)data, n, DBSQL_TRANSIENT,
                              DBSQL_UTF8);
        break;
      }
    }

  }
}

#ifndef DBSQL_OMIT_AUTHORIZATION
/*
** This is the authentication function.  It appends the authentication
** type code and the two arguments to cmd[] then invokes the result
** on the interpreter.  The reply is examined to determine if the
** authentication fails or succeeds.
*/
static int auth_callback(
  void *arg,
  int code,
  const char *arg1,
  const char *arg2,
  const char *arg3,
  const char *arg4
){
  const char *z_code;
  Tcl_DString str;
  int rc;
  const char *reply;
  /* EVIDENCE-OF: R-38590-62769 The first parameter to the authorizer
  ** callback is a copy of the third parameter to the
  ** dbsql_set_authorizer() interface.
  */
  dbsql_db *db = (dbsql_db*)arg;
  if( db->disable_auth ) return DBSQL_OK;

  /* EVIDENCE-OF: R-56518-44310 The second parameter to the callback is an
  ** integer action code that specifies the particular action to be
  ** authorized. */
  switch( code ){
    case DBSQL_COPY              : z_code="DBSQL_COPY"; break;
    case DBSQL_CREATE_INDEX      : z_code="DBSQL_CREATE_INDEX"; break;
    case DBSQL_CREATE_TABLE      : z_code="DBSQL_CREATE_TABLE"; break;
    case DBSQL_CREATE_TEMP_INDEX : z_code="DBSQL_CREATE_TEMP_INDEX"; break;
    case DBSQL_CREATE_TEMP_TABLE : z_code="DBSQL_CREATE_TEMP_TABLE"; break;
    case DBSQL_CREATE_TEMP_TRIGGER: z_code="DBSQL_CREATE_TEMP_TRIGGER"; break;
    case DBSQL_CREATE_TEMP_VIEW  : z_code="DBSQL_CREATE_TEMP_VIEW"; break;
    case DBSQL_CREATE_TRIGGER    : z_code="DBSQL_CREATE_TRIGGER"; break;
    case DBSQL_CREATE_VIEW       : z_code="DBSQL_CREATE_VIEW"; break;
    case DBSQL_DELETE            : z_code="DBSQL_DELETE"; break;
    case DBSQL_DROP_INDEX        : z_code="DBSQL_DROP_INDEX"; break;
    case DBSQL_DROP_TABLE        : z_code="DBSQL_DROP_TABLE"; break;
    case DBSQL_DROP_TEMP_INDEX   : z_code="DBSQL_DROP_TEMP_INDEX"; break;
    case DBSQL_DROP_TEMP_TABLE   : z_code="DBSQL_DROP_TEMP_TABLE"; break;
    case DBSQL_DROP_TEMP_TRIGGER : z_code="DBSQL_DROP_TEMP_TRIGGER"; break;
    case DBSQL_DROP_TEMP_VIEW    : z_code="DBSQL_DROP_TEMP_VIEW"; break;
    case DBSQL_DROP_TRIGGER      : z_code="DBSQL_DROP_TRIGGER"; break;
    case DBSQL_DROP_VIEW         : z_code="DBSQL_DROP_VIEW"; break;
    case DBSQL_INSERT            : z_code="DBSQL_INSERT"; break;
    case DBSQL_PRAGMA            : z_code="DBSQL_PRAGMA"; break;
    case DBSQL_READ              : z_code="DBSQL_READ"; break;
    case DBSQL_SELECT            : z_code="DBSQL_SELECT"; break;
    case DBSQL_TRANSACTION       : z_code="DBSQL_TRANSACTION"; break;
    case DBSQL_UPDATE            : z_code="DBSQL_UPDATE"; break;
    case DBSQL_ATTACH            : z_code="DBSQL_ATTACH"; break;
    case DBSQL_DETACH            : z_code="DBSQL_DETACH"; break;
    case DBSQL_ALTER_TABLE       : z_code="DBSQL_ALTER_TABLE"; break;
    case DBSQL_REINDEX           : z_code="DBSQL_REINDEX"; break;
    case DBSQL_ANALYZE           : z_code="DBSQL_ANALYZE"; break;
    case DBSQL_CREATE_VTABLE     : z_code="DBSQL_CREATE_VTABLE"; break;
    case DBSQL_DROP_VTABLE       : z_code="DBSQL_DROP_VTABLE"; break;
    case DBSQL_FUNCTION          : z_code="DBSQL_FUNCTION"; break;
    case DBSQL_SAVEPOINT         : z_code="DBSQL_SAVEPOINT"; break;
    case DBSQL_RECURSIVE         : z_code="DBSQL_RECURSIVE"; break;
    default                       : z_code="????"; break;
  }
  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, db->auth, -1);
  Tcl_DStringAppendElement(&str, z_code);
  Tcl_DStringAppendElement(&str, arg1 ? arg1 : "");
  Tcl_DStringAppendElement(&str, arg2 ? arg2 : "");
  Tcl_DStringAppendElement(&str, arg3 ? arg3 : "");
  Tcl_DStringAppendElement(&str, arg4 ? arg4 : "");
  rc = Tcl_GlobalEval(db->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  reply = rc==TCL_OK ? Tcl_GetStringResult(db->interp) : "DBSQL_DENY";
  if( strcmp(reply,"DBSQL_OK")==0 ){
    rc = DBSQL_OK;
  }else if( strcmp(reply,"DBSQL_DENY")==0 ){
    rc = DBSQL_DENY;
  }else if( strcmp(reply,"DBSQL_IGNORE")==0 ){
    rc = DBSQL_IGNORE;
  }else{
    rc = 999;
  }
  return rc;
}
#endif /* DBSQL_OMIT_AUTHORIZATION */

#if 0
/*
** This routine reads a line of text from FILE in, stores
** the text in memory obtained from malloc() and returns a pointer
** to the text.  NULL is returned at end of file, or if malloc()
** fails.
**
** The interface is like "readline" but no command-line editing
** is done.
**
** copied from shell.c from '.import' command
*/
static char *local_getline(char *prompt, FILE *in){
  char *line;
  int n_line;
  int n;

  n_line = 100;
  line = malloc( n_line );
  if( line==0 ) return 0;
  n = 0;
  while( 1 ){
    if( n+100>n_line ){
      n_line = n_line*2 + 100;
      line = realloc(line, n_line);
      if( line==0 ) return 0;
    }
    if( fgets(&line[n], n_line - n, in)==0 ){
      if( n==0 ){
        free(line);
        return 0;
      }
      line[n] = 0;
      break;
    }
    while( line[n] ){ n++; }
    if( n>0 && line[n-1]=='\n' ){
      n--;
      line[n] = 0;
      break;
    }
  }
  line = realloc( line, n+1 );
  return line;
}
#endif


/*
** This function is part of the implementation of the command:
**
**   $db transaction [-deferred|-immediate|-exclusive] SCRIPT
**
** It is invoked after evaluating the script SCRIPT to commit or rollback
** the transaction or savepoint opened by the [transaction] command.
*/
static int DBSQL_TCLAPI __db_trans_post_cmd(
  ClientData data[],                   /* data[0] is the Sqlite3Db* for $db */
  Tcl_Interp *interp,                  /* Tcl interpreter */
  int result                           /* Result of evaluating SCRIPT */
){
  static const char *const ends[] = {
    "RELEASE _tcl_transaction",        /* rc==TCL_ERROR, n_transaction!=0 */
    "COMMIT",                          /* rc!=TCL_ERROR, n_transaction==0 */
    "ROLLBACK TO _tcl_transaction ; RELEASE _tcl_transaction",
    "ROLLBACK"                         /* rc==TCL_ERROR, n_transaction==0 */
  };
  dbsql_db *db = (dbsql_db*)data[0];
  int rc = result;
  const char *end;

  db->n_transaction--;
  end = ends[(rc==TCL_ERROR)*2 + (db->n_transaction==0)];

  db->disable_auth++;
  if( dbsql_exec(db->db, end, 0, 0, 0) ){
      /* This is a tricky scenario to handle. The most likely cause of an
      ** error is that the exec() above was an attempt to commit the
      ** top-level transaction that returned DBSQL_BUSY. Or, less likely,
      ** that an IO-error has occurred. In either case, throw a Tcl exception
      ** and try to rollback the transaction.
      **
      ** But it could also be that the user executed one or more BEGIN,
      ** COMMIT, SAVEPOINT, RELEASE or ROLLBACK commands that are confusing
      ** this method's logic. Not clear how this would be best handled.
      */
    if( rc!=TCL_ERROR ){
      Tcl_AppendResult(interp, dbsql_errmsg(db->db), (char*)0);
      rc = TCL_ERROR;
    }
    dbsql_exec(db->db, "ROLLBACK", 0, 0, 0);
  }
  db->disable_auth--;

  __del_database_ref(db);
  return rc;
}

/*
** Unless DBSQL_TEST is defined, this function is a simple wrapper around
** dbsql_prepare_v2(). If DBSQL_TEST is defined, then it uses either
** dbsql_prepare_v2() or legacy interface dbsql_prepare(), depending
** on whether or not the [db_use_legacy_prepare] command has been used to
** configure the connection.
*/
static int __db_prepare(
  dbsql_db *db,                  /* Database object */
  const char *sql,               /* SQL to compile */
  dbsql_stmt_t **stmt,          /* OUT: Prepared statement */
  const char **out              /* OUT: Pointer to next SQL statement */
){
  unsigned int prep_flags = 0;
#ifdef DBSQL_TEST
  if( db->legacy_prepare ){
    return dbsql_prepare(db->db, sql, -1, stmt, out);
  }
#endif
  /* If the statement cache is large, use the DBSQL_PREPARE_PERSISTENT
  ** flags, which uses less lookaside memory.  But if the cache is small,
  ** omit that flag to make full use of lookaside */
  if( db->max_stmt>5 ) prep_flags = DBSQL_PREPARE_PERSISTENT;

  return dbsql_prepare_v3(db->db, sql, -1, prep_flags, stmt, out);
}

/*
** Search the cache for a prepared-statement object that implements the
** first SQL statement in the buffer pointed to by parameter in. If
** no such prepared-statement can be found, allocate and prepare a new
** one. In either case, bind the current values of the relevant Tcl
** variables to any $var, :var or @var variables in the statement. Before
** returning, set *pre_stmt to point to the prepared-statement object.
**
** Output parameter *out is set to point to the next SQL statement in
** buffer in, or to the '\0' byte at the end of in if there is no
** next statement.
**
** If successful, TCL_OK is returned. Otherwise, TCL_ERROR is returned
** and an error message loaded into interpreter db->interp.
*/
static int __db_prepare_and_bind(
  dbsql_db *db,                  /* Database object */
  char const *in,                /* SQL to compile */
  char const **out,             /* OUT: Pointer to next SQL statement */
  sql_prepared_stmt_t **pp_pre_stmt     /* OUT: Object used to cache statement */
){
  const char *sql = in;         /* Pointer to first SQL statement in in */
  dbsql_stmt_t *stmt = 0;        /* Prepared statement object */
  sql_prepared_stmt_t *pre_stmt;      /* Pointer to cached statement */
  int n_sql;                       /* Length of sql in bytes */
  int n_var = 0;                   /* Number of variables in statement */
  int i_parm = 0;                  /* Next free entry in parms */
  char c;
  int i;
  int need_result_reset = 0;        /* Need to invoke Tcl_ResetResult() */
  int rc = DBSQL_OK;             /* Value to return */
  Tcl_Interp *interp = db->interp;

  *pp_pre_stmt = 0;

  /* Trim spaces from the start of sql and calculate the remaining length. */
  while( (c = sql[0])==' ' || c=='\t' || c=='\r' || c=='\n' ){ sql++; }
  n_sql = strlen30(sql);

  for(pre_stmt = db->stmt_list; pre_stmt; pre_stmt=pre_stmt->next){
    int n = pre_stmt->n_sql;
    if( n_sql>=n
        && memcmp(pre_stmt->z_sql, sql, n)==0
        && (sql[n]==0 || sql[n-1]==';')
    ){
      stmt = pre_stmt->stmt;
      *out = &sql[pre_stmt->n_sql];

      /* When a prepared statement is found, unlink it from the
      ** cache list.  It will later be added back to the beginning
      ** of the cache list in order to implement LRU replacement.
      */
      if( pre_stmt->prev ){
        pre_stmt->prev->next = pre_stmt->next;
      }else{
        db->stmt_list = pre_stmt->next;
      }
      if( pre_stmt->next ){
        pre_stmt->next->prev = pre_stmt->prev;
      }else{
        db->stmt_last = pre_stmt->prev;
      }
      db->n_stmt--;
      n_var = dbsql_bind_parameter_count(stmt);
      break;
    }
  }

  /* If no prepared statement was found. Compile the SQL text. Also allocate
  ** a new sql_prepared_stmt_t structure.  */
  if( pre_stmt==0 ){
    int n_byte;

    if( DBSQL_OK!=__db_prepare(db, sql, &stmt, out) ){
      Tcl_SetObjResult(interp, Tcl_NewStringObj(dbsql_errmsg(db->db), -1));
      return TCL_ERROR;
    }
    if( stmt==0 ){
      if( DBSQL_OK!=dbsql_errcode(db->db) ){
        /* A compile-time error in the statement. */
        Tcl_SetObjResult(interp, Tcl_NewStringObj(dbsql_errmsg(db->db), -1));
        return TCL_ERROR;
      }else{
        /* The statement was a no-op.  Continue to the next statement
        ** in the SQL string.
        */
        return TCL_OK;
      }
    }

    assert( pre_stmt==0 );
    n_var = dbsql_bind_parameter_count(stmt);
    n_byte = sizeof(sql_prepared_stmt_t) + n_var*sizeof(Tcl_Obj *);
    pre_stmt = (sql_prepared_stmt_t*)Tcl_Alloc(n_byte);
    memset(pre_stmt, 0, n_byte);

    pre_stmt->stmt = stmt;
    pre_stmt->n_sql = (int)(*out - sql);
    pre_stmt->z_sql = dbsql_sql(stmt);
    pre_stmt->parms = (Tcl_Obj **)&pre_stmt[1];
#ifdef DBSQL_TEST
    if( pre_stmt->z_sql==0 ){
      char *copy = Tcl_Alloc(pre_stmt->n_sql + 1);
      memcpy(copy, sql, pre_stmt->n_sql);
      copy[pre_stmt->n_sql] = '\0';
      pre_stmt->z_sql = copy;
    }
#endif
  }
  assert( pre_stmt );
  assert( strlen30(pre_stmt->z_sql)==pre_stmt->n_sql );
  assert( 0==memcmp(pre_stmt->z_sql, sql, pre_stmt->n_sql) );

  /* Bind values to parameters that begin with $ or : */
  for(i=1; i<=n_var; i++){
    const char *z_var = dbsql_bind_parameter_name(stmt, i);
    if( z_var!=0 && (z_var[0]=='$' || z_var[0]==':' || z_var[0]=='@') ){
      Tcl_Obj *var = Tcl_GetVar2Ex(interp, &z_var[1], 0, 0);
      if( var==0 && db->bind_fallback!=0 ){
        Tcl_Obj *cmd;
        int rx;
        cmd = Tcl_NewStringObj(db->bind_fallback, -1);
        Tcl_IncrRefCount(cmd);
        Tcl_ListObjAppendElement(interp, cmd, Tcl_NewStringObj(z_var,-1));
        if( need_result_reset ) Tcl_ResetResult(interp);
        need_result_reset = 1;
        rx = Tcl_EvalObjEx(interp, cmd, TCL_EVAL_DIRECT);
        Tcl_DecrRefCount(cmd);
        if( rx==TCL_OK ){
          var = Tcl_GetObjResult(interp);
        }else if( rx==TCL_ERROR ){
          rc = TCL_ERROR;
          break;
        }else{
          var = 0;
        }
      }
      if( var ){
        TCL_SIZE n;
        u_int8_t *data;
        const char *type = (var->typePtr ? var->typePtr->name : "");
        c = type[0];
        if( z_var[0]=='@' ||
           (c=='b' && strcmp(type,"bytearray")==0 && var->bytes==0) ){
          /* Load a BLOB type if the Tcl variable is a bytearray and
          ** it has no string representation or the host
          ** parameter name begins with "@". */
          data = Tcl_GetByteArrayFromObj(var, &n);
          dbsql_bind_blob(stmt, i, data, n, DBSQL_STATIC);
          Tcl_IncrRefCount(var);
          pre_stmt->parms[i_parm++] = var;
        }else if( c=='b' && var->bytes==0 
               && (strcmp(type,"booleanString")==0
                   || strcmp(type,"boolean")==0)
        ){
          int nn;
          Tcl_GetBooleanFromObj(interp, var, &nn);
          dbsql_bind_int(stmt, i, nn);
        }else if( c=='d' && strcmp(type,"double")==0 ){
          double r;
          Tcl_GetDoubleFromObj(interp, var, &r);
          dbsql_bind_double(stmt, i, r);
        }else if( (c=='w' && strcmp(type,"wideInt")==0) ||
              (c=='i' && strcmp(type,"int")==0) ){
          Tcl_WideInt v;
          Tcl_GetWideIntFromObj(interp, var, &v);
          dbsql_bind_int64(stmt, i, v);
        }else{
          data = (unsigned char *)Tcl_GetStringFromObj(var, &n);
          dbsql_bind_text64(stmt, i, (char *)data, n, DBSQL_STATIC,
                              DBSQL_UTF8);
          Tcl_IncrRefCount(var);
          pre_stmt->parms[i_parm++] = var;
        }
      }else{
        dbsql_bind_null(stmt, i);
      }
      if( need_result_reset ) Tcl_ResetResult(db->interp);
    }
  }
  pre_stmt->n_parm = i_parm;
  *pp_pre_stmt = pre_stmt;
  if( need_result_reset && rc==TCL_OK ) Tcl_ResetResult(db->interp);

  return rc;
}

/*
** Release a statement reference obtained by calling __db_prepare_and_bind().
** There should be exactly one call to this function for each call to
** __db_prepare_and_bind().
**
** If the discard parameter is non-zero, then the statement is deleted
** immediately. Otherwise it is added to the LRU list and may be returned
** by a subsequent call to __db_prepare_and_bind().
*/
static void __db_release_stmt(
  dbsql_db *db,                  /* Database handle */
  sql_prepared_stmt_t *pre_stmt,      /* Prepared statement handle to release */
  int discard                     /* True to delete (not cache) the pre_stmt */
){
  int i;

  /* Free the bound string and blob parameters */
  for(i=0; i<pre_stmt->n_parm; i++){
    Tcl_DecrRefCount(pre_stmt->parms[i]);
  }
  pre_stmt->n_parm = 0;

  if( db->max_stmt<=0 || discard ){
    /* If the cache is turned off, deallocated the statement */
    __db_free_stmt(pre_stmt);
  }else{
    /* Add the prepared statement to the beginning of the cache list. */
    pre_stmt->next = db->stmt_list;
    pre_stmt->prev = 0;
    if( db->stmt_list ){
     db->stmt_list->prev = pre_stmt;
    }
    db->stmt_list = pre_stmt;
    if( db->stmt_last==0 ){
      assert( db->n_stmt==0 );
      db->stmt_last = pre_stmt;
    }else{
      assert( db->n_stmt>0 );
    }
    db->n_stmt++;

    /* If we have too many statement in cache, remove the surplus from
    ** the end of the cache list.  */
    while( db->n_stmt>db->max_stmt ){
      sql_prepared_stmt_t *last = db->stmt_last;
      db->stmt_last = last->prev;
      db->stmt_last->next = 0;
      db->n_stmt--;
      __db_free_stmt(last);
    }
  }
}

/*
** Structure used with db_eval_xxx() functions:
**
**   __db_eval_init()
**   __db_eval_step()
**   __db_eval_finalize()
**   __db_eval_row_info()
**   __db_eval_column_value()
*/
typedef struct db_eval_context_t db_eval_context_t;
struct db_eval_context_t {
  dbsql_db *db;                  /* Database handle */
  Tcl_Obj *sql;                  /* Object holding string sql */
  const char *z_sql;               /* Remaining SQL to execute */
  sql_prepared_stmt_t *pre_stmt;      /* Current statement */
  int n_col;                       /* Number of columns returned by stmt */
  int eval_flags;                  /* Flags used */
  Tcl_Obj *var_name;              /* Name of target array/dict variable */
  Tcl_Obj **col_names;            /* Array of column names */
};

#define DBSQL_EVAL_WITHOUTNULLS  0x00001  /* Unset array(*) for NULL */
#define DBSQL_EVAL_ASDICT        0x00002  /* Use dict instead of array */

/*
** Release any cache of column names currently held as part of
** the db_eval_context_t structure passed as the first argument.
*/
static void __db_release_column_names(db_eval_context_t *p){
  if( p->col_names ){
    int i;
    for(i=0; i<p->n_col; i++){
      Tcl_DecrRefCount(p->col_names[i]);
    }
    Tcl_Free((char *)p->col_names);
    p->col_names = 0;
  }
  p->n_col = 0;
}

/*
** Initialize a db_eval_context_t structure.
**
** If var_name is not NULL, then it contains the name of a Tcl array
** variable. The "*" member of this array is set to a list containing
** the names of the columns returned by the statement as part of each
** call to __db_eval_step(), in order from left to right. e.g. if the names
** of the returned columns are a, b and c, it does the equivalent of the
** tcl command:
**
**     set ${var_name}(*) {a b c}
*/
static void __db_eval_init(
  db_eval_context_t *p,               /* Pointer to structure to initialize */
  dbsql_db *db,                  /* Database handle */
  Tcl_Obj *sql,                  /* Object containing SQL script */
  Tcl_Obj *var_name,              /* Name of Tcl array to set (*) element of */
  int eval_flags                   /* Flags controlling evaluation */
){
  memset(p, 0, sizeof(db_eval_context_t));
  p->db = db;
  p->z_sql = Tcl_GetString(sql);
  p->sql = sql;
  Tcl_IncrRefCount(sql);
  if( var_name ){
    p->var_name = var_name;
    Tcl_IncrRefCount(var_name);
  }
  p->eval_flags = eval_flags;
  __add_database_ref(p->db);
}

/*
** Obtain information about the row that the db_eval_context_t passed as the
** first argument currently points to.
*/
static void __db_eval_row_info(
  db_eval_context_t *p,               /* Evaluation context */
  int *pn_col,                     /* OUT: Number of column names */
  Tcl_Obj ***pap_col_name           /* OUT: Array of column names */
){
  /* Compute column names */
  if( 0==p->col_names ){
    dbsql_stmt_t *stmt = p->pre_stmt->stmt;
    int i;                        /* Iterator variable */
    int n_col;                     /* Number of columns returned by stmt */
    Tcl_Obj **col_names = 0;      /* Array of column names */

    p->n_col = n_col = dbsql_column_count(stmt);
    if( n_col>0 && (pap_col_name || p->var_name) ){
      col_names = (Tcl_Obj**)Tcl_Alloc( sizeof(Tcl_Obj*)*n_col );
      for(i=0; i<n_col; i++){
        col_names[i] = Tcl_NewStringObj(dbsql_column_name(stmt,i), -1);
        Tcl_IncrRefCount(col_names[i]);
      }
      p->col_names = col_names;
    }

    /* If results are being stored in a variable then create the
    ** array(*) or dict(*) entry for that variable.
    */
    if( p->var_name ){
      Tcl_Interp *interp = p->db->interp;
      Tcl_Obj *col_list = Tcl_NewObj();
      Tcl_Obj *star = Tcl_NewStringObj("*", -1);

      Tcl_IncrRefCount(col_list);
      Tcl_IncrRefCount(star);
      for(i=0; i<n_col; i++){
        Tcl_ListObjAppendElement(interp, col_list, col_names[i]);
      }
      if( 0==(DBSQL_EVAL_ASDICT & p->eval_flags) ){
        Tcl_ObjSetVar2(interp, p->var_name, star, col_list, 0);
      }else{
        Tcl_Obj * dict = Tcl_ObjGetVar2(interp, p->var_name, NULL, 0);
        if( !dict ){
          dict = Tcl_NewDictObj();
        }else if( Tcl_IsShared(dict) ){
          dict = Tcl_DuplicateObj(dict);
        }
        if( Tcl_DictObjPut(interp, dict, star, col_list)==TCL_OK ){
          Tcl_ObjSetVar2(interp, p->var_name, NULL, dict, 0);
        }
        TCL_BOUNCE_REF_COUNT(dict);
      }
      Tcl_DecrRefCount(star);
      Tcl_DecrRefCount(col_list);
    }
  }

  if( pap_col_name ){
    *pap_col_name = p->col_names;
  }
  if( pn_col ){
    *pn_col = p->n_col;
  }
}

/*
** Return one of TCL_OK, TCL_BREAK or TCL_ERROR. If TCL_ERROR is
** returned, then an error message is stored in the interpreter before
** returning.
**
** A return value of TCL_OK means there is a row of data available. The
** data may be accessed using __db_eval_row_info() and __db_eval_column_value(). This
** is analogous to a return of DBSQL_ROW from dbsql_step(). If TCL_BREAK
** is returned, then the SQL script has finished executing and there are
** no further rows available. This is similar to DBSQL_DONE.
*/
static int __db_eval_step(db_eval_context_t *p){
  const char *prev_sql = 0;       /* Previous value of p->sql */

  while( p->z_sql[0] || p->pre_stmt ){
    int rc;
    if( p->pre_stmt==0 ){
      prev_sql = (p->z_sql==prev_sql ? 0 : p->z_sql);
      rc = __db_prepare_and_bind(p->db, p->z_sql, &p->z_sql, &p->pre_stmt);
      if( rc!=TCL_OK ) return rc;
    }else{
      int rcs;
      dbsql_db *db = p->db;
      sql_prepared_stmt_t *pre_stmt = p->pre_stmt;
      dbsql_stmt_t *stmt = pre_stmt->stmt;

      rcs = dbsql_step(stmt);
      if( rcs==DBSQL_ROW ){
        return TCL_OK;
      }
      if( p->var_name ){
        __db_eval_row_info(p, 0, 0);
      }
      rcs = dbsql_reset(stmt);

      db->n_step = dbsql_stmt_status(stmt,DBSQL_STMTSTATUS_FULLSCAN_STEP,1);
      db->n_sort = dbsql_stmt_status(stmt,DBSQL_STMTSTATUS_SORT,1);
      db->n_index = dbsql_stmt_status(stmt,DBSQL_STMTSTATUS_AUTOINDEX,1);
      db->n_vm_step = dbsql_stmt_status(stmt,DBSQL_STMTSTATUS_VM_STEP,1);
      __db_release_column_names(p);
      p->pre_stmt = 0;

      if( rcs!=DBSQL_OK ){
        /* If a run-time error occurs, report the error and stop reading
        ** the SQL.  */
        __db_release_stmt(db, pre_stmt, 1);
#if DBSQL_TEST
        if( p->db->legacy_prepare && rcs==DBSQL_SCHEMA && prev_sql ){
          /* If the runtime error was an DBSQL_SCHEMA, and the database
          ** handle is configured to use the legacy dbsql_prepare()
          ** interface, retry prepare()/step() on the same SQL statement.
          ** This only happens once. If there is a second DBSQL_SCHEMA
          ** error, the error will be returned to the caller. */
          p->z_sql = prev_sql;
          continue;
        }
#endif
        Tcl_SetObjResult(db->interp,
                         Tcl_NewStringObj(dbsql_errmsg(db->db), -1));
        return TCL_ERROR;
      }else{
        __db_release_stmt(db, pre_stmt, 0);
      }
    }
  }

  /* Finished */
  return TCL_BREAK;
}

/*
** Free all resources currently held by the db_eval_context_t structure passed
** as the first argument. There should be exactly one call to this function
** for each call to __db_eval_init().
*/
static void __db_eval_finalize(db_eval_context_t *p){
  if( p->pre_stmt ){
    dbsql_reset(p->pre_stmt->stmt);
    __db_release_stmt(p->db, p->pre_stmt, 0);
    p->pre_stmt = 0;
  }
  if( p->var_name ){
    Tcl_DecrRefCount(p->var_name);
    p->var_name = 0;
  }
  Tcl_DecrRefCount(p->sql);
  __db_release_column_names(p);
  __del_database_ref(p->db);
}

/*
** Return a pointer to a Tcl_Obj structure with ref-count 0 that contains
** the value for the i_col'th column of the row currently pointed to by
** the db_eval_context_t structure passed as the first argument.
*/
static Tcl_Obj *__db_eval_column_value(db_eval_context_t *p, int i_col){
  dbsql_stmt_t *stmt = p->pre_stmt->stmt;
  switch( dbsql_column_type(stmt, i_col) ){
    case DBSQL_BLOB: {
      int bytes = dbsql_column_bytes(stmt, i_col);
      const char *blob = dbsql_column_blob(stmt, i_col);
      if( !blob ) bytes = 0;
      return Tcl_NewByteArrayObj((u_int8_t*)blob, bytes);
    }
    case DBSQL_INTEGER: {
      int64_t v = dbsql_column_int64(stmt, i_col);
      if( v>=-2147483647 && v<=2147483647 ){
        return Tcl_NewIntObj((int)v);
      }else{
        return Tcl_NewWideIntObj(v);
      }
    }
    case DBSQL_FLOAT: {
      return Tcl_NewDoubleObj(dbsql_column_double(stmt, i_col));
    }
    case DBSQL_NULL: {
      return Tcl_NewStringObj(p->db->null, -1);
    }
  }

  return Tcl_NewStringObj((char*)dbsql_column_text(stmt, i_col), -1);
}

/*
** If using Tcl version 8.6 or greater, use the NR functions to avoid
** recursive evaluation of scripts by the [db eval] and [db trans]
** commands. Even if the headers used while compiling the extension
** are 8.6 or newer, the code still tests the Tcl version at runtime.
** This allows stubs-enabled builds to be used with older Tcl libraries.
*/
#if TCL_MAJOR_VERSION>8 || !defined(TCL_MINOR_VERSION) \
                        || TCL_MINOR_VERSION>=6
# define DBSQL_TCL_NRE 1
static int DB_USE_NRE(void){
  int major, minor;
  Tcl_GetVersion(&major, &minor, 0, 0);
  return( (major==8 && minor>=6) || major>8 );
}
#else
/*
** Compiling using headers earlier than 8.6. In this case NR cannot be
** used, so DB_USE_NRE() to always return zero. Add #defines for the other
** Tcl_NRxxx() functions to prevent them from causing compilation errors,
** even though the only invocations of them are within conditional blocks
** of the form:
**
**   if( DB_USE_NRE() ) { ... }
*/
# define DBSQL_TCL_NRE 0
# define DB_USE_NRE() 0
# define Tcl_NRAddCallback(a,b,c,d,e,f) (void)0
# define Tcl_NREvalObj(a,b,c) 0
# define Tcl_NRCreateCommand(a,b,c,d,e,f) (void)0
#endif

/*
** This function is part of the implementation of the command:
**
**   $db eval SQL ?TGT-NAME? SCRIPT
*/
static int DBSQL_TCLAPI __db_eval_next_cmd(
  ClientData data[],                   /* data[0] is the (db_eval_context_t*) */
  Tcl_Interp *interp,                  /* Tcl interpreter */
  int result                           /* Result so far */
){
  int rc = result;                     /* Return code */

  /* The first element of the data[] array is a pointer to a db_eval_context_t
  ** structure allocated using Tcl_Alloc(). The second element of data[]
  ** is a pointer to a Tcl_Obj containing the script to run for each row
  ** returned by the queries encapsulated in data[0]. */
  db_eval_context_t *p = (db_eval_context_t *)data[0];
  Tcl_Obj * const script = (Tcl_Obj *)data[1];
  Tcl_Obj * const var_name = p->var_name;

  while( (rc==TCL_OK || rc==TCL_CONTINUE) && TCL_OK==(rc = __db_eval_step(p)) ){
    int i;
    int n_col;
    Tcl_Obj **col_names;
    __db_eval_row_info(p, &n_col, &col_names);
    for(i=0; i<n_col; i++){
      if( var_name==0 ){
        Tcl_ObjSetVar2(interp, col_names[i], 0, __db_eval_column_value(p,i), 0);
      }else if( (p->eval_flags & DBSQL_EVAL_WITHOUTNULLS)!=0
             && dbsql_column_type(p->pre_stmt->stmt, i)==DBSQL_NULL
      ){
        /* Remove NULL-containing column from the target container... */
        if( 0==(DBSQL_EVAL_ASDICT & p->eval_flags) ){
          /* Target is an array */
          Tcl_UnsetVar2(interp, Tcl_GetString(var_name),
                        Tcl_GetString(col_names[i]), 0);
        }else{
          /* Target is a dict */
          Tcl_Obj *dict = Tcl_ObjGetVar2(interp, var_name, NULL, 0);
          if( dict ){
            if( Tcl_IsShared(dict) ){
              dict = Tcl_DuplicateObj(dict);
            }
            if( Tcl_DictObjRemove(interp, dict, col_names[i])==TCL_OK ){
              Tcl_ObjSetVar2(interp, var_name, NULL, dict, 0);
            }
            TCL_BOUNCE_REF_COUNT(dict);
          }
        }
      }else if( 0==(DBSQL_EVAL_ASDICT & p->eval_flags) ){
        /* Target is an array: set target(col_name) = col_value */
        Tcl_ObjSetVar2(interp, var_name, col_names[i],
                       __db_eval_column_value(p,i), 0);
      }else{
        /* Target is a dict: set target(col_name) = col_value */
        Tcl_Obj *dict = Tcl_ObjGetVar2(interp, var_name, NULL, 0);
        if( !dict ){
          dict = Tcl_NewDictObj();
        }else if( Tcl_IsShared(dict) ){
          dict = Tcl_DuplicateObj(dict);
        }
        if( Tcl_DictObjPut(interp, dict, col_names[i],
                           __db_eval_column_value(p,i))==TCL_OK ){
          Tcl_ObjSetVar2(interp, var_name, NULL, dict, 0);
        }
        TCL_BOUNCE_REF_COUNT(dict);
      }
    }

    /* The required interpreter variables are now populated with the data
    ** from the current row. If using NRE, schedule callbacks to evaluate
    ** script script, then to invoke this function again to fetch the next
    ** row (or clean up if there is no next row or the script throws an
    ** exception). After scheduling the callbacks, return control to the
    ** caller.
    **
    ** If not using NRE, evaluate script directly and continue with the
    ** next iteration of this while(...) loop.  */
    if( DB_USE_NRE() ){
      Tcl_NRAddCallback(interp, __db_eval_next_cmd, (void*)p, (void*)script, 0, 0);
      return Tcl_NREvalObj(interp, script, 0);
    }else{
      rc = Tcl_EvalObjEx(interp, script, 0);
    }
  }

  Tcl_DecrRefCount(script);
  __db_eval_finalize(p);
  Tcl_Free((char *)p);

  if( rc==TCL_OK || rc==TCL_BREAK ){
    Tcl_ResetResult(interp);
    rc = TCL_OK;
  }
  return rc;
}

/*
** This function is used by the implementations of the following database
** handle sub-commands:
**
**   $db update_hook ?SCRIPT?
**   $db wal_hook ?SCRIPT?
**   $db commit_hook ?SCRIPT?
**   $db preupdate hook ?SCRIPT?
*/
static void __db_hook_cmd(
  Tcl_Interp *interp,             /* Tcl interpreter */
  dbsql_db *p_db,                  /* Database handle */
  Tcl_Obj *arg,                  /* SCRIPT argument (or NULL) */
  Tcl_Obj **hook                /* Pointer to member of dbsql_db */
){
  DBSQL *db = p_db->db;

  if( *hook ){
    Tcl_SetObjResult(interp, *hook);
    if( arg ){
      Tcl_DecrRefCount(*hook);
      *hook = 0;
    }
  }
  if( arg ){
    assert( !(*hook) );
    if( Tcl_GetString(arg)[0] ){
      *hook = arg;
      Tcl_IncrRefCount(*hook);
    }
  }

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  dbsql_preupdate_hook(db, (p_db->pre_update_hook?__db_pre_update_handler:0), p_db);
#endif
  dbsql_update_hook(db, (p_db->update_hook?__db_update_handler:0), p_db);
  dbsql_rollback_hook(db, (p_db->rollback_hook?__db_rollback_handler:0), p_db);
  dbsql_wal_hook(db, (p_db->wal_hook?__db_wal_handler:0), p_db);
}

/*
** Implementation of the "db format" command.
**
** Based on provided options, format the results of the SQL statement(s)
** provided into human-readable form using the Query Result Formatter (QRF)
** and return the resuling text.
**
** Syntax:    db format OPTIONS SQL
**
** OPTIONS may be:
**
**     -style ("auto"|"box"|"column"|...)      Output style
**     -esc ("auto"|"off"|"ascii"|"symbol")    How to deal with ctrl chars
**     -text ("auto"|"off"|"sql"|"csv"|...)    How to escape TEXT values
**     -title ("auto"|"off"|"sql"|...|"off")   How to escape column names
**     -blob ("auto"|"text"|"sql"|...)         How to escape BLOB values
**     -wordwrap ("auto"|"off"|"on")           Try to wrap at word boundry?
**     -textjsonb ("auto"|"off"|"on")          Auto-convert JSONB to text?
**     -splitcolumn ("auto"|"off"|"on")        Enable split-column mode
**     -defaultalign ("auto"|"left"|...)       Default alignment
**     -titalalign ("auto"|"left"|"right"|...) Default column name alignment
**     -border ("auto"|"off"|"on")             Border for box and table styles
**     -wrap NUMBER                            Max width of any single column
**     -screenwidth NUMBER                     Width of the display TTY
**     -linelimit NUMBER                       Max lines for any cell
**     -charlimit NUMBER                       Content truncated to this size
**     -titlelimit NUMBER                      Max width of column titles
**     -multiinsert NUMBER                     Multi-row INSERT byte size
**     -align LIST-OF-ALIGNMENT                Alignment of columns
**     -widths LIST-OF-NUMBERS                 Widths for individual columns
**     -columnsep TEXT                         column_t separator text
**     -rowsep TEXT                            Row separator text
**     -tablename TEXT                         table_t name for style "insert"
**     -null TEXT                              Text for NULL values
**
** A mapping from TCL "format" command options to dbsql_qrf_spec fields
** is below.  Use this to reference the QRF documentation:
**
**     TCL Option        spec field
**     ----------        ----------
**     -style            style
**     -esc              esc
**     -text             text
**     -title            title, title
**     -blob             blob
**     -wordwrap         word_wrap
**     -textjsonb        text_jsonb
**     -splitcolumn      split_column
**     -defaultalign     dflt_align
**     -titlealign       title_align
**     -border           border
**     -wrap             n_wrap
**     -screenwidth      n_screen_width
**     -linelimit        n_line_limit
**     -charlimit        n_char_limit
**     -titlelimit       n_title_limit
**     -multiinsert      n_multi_insert
**     -align            n_align, aligns
**     -widths           n_width, widths
**     -columnsep        column_sep
**     -rowsep           row_sep
**     -tablename        table_name
**     -null             null
*/
static int __db_qrf(dbsql_db *db, int objc, Tcl_Obj *const*objv){
#ifndef DBSQL_QRF_H
  Tcl_SetResult(db->interp, "QRF not available in this build", TCL_VOLATILE);
  return TCL_ERROR;
#else
  char *result = 0;             /* Result to be returned */
  const char *sql = 0;          /* SQL to run */
  int i;                         /* Loop counter */
  int rc;                        /* Result code */
  dbsql_qrf_spec qrf;          /* Formatting spec */
  static const char *aligns[] = {
    "auto",           "bottom",          "c",
    "center",         "e",               "left",
    "middle",         "n",               "ne",
    "nw",             "right",           "s",
    "se",             "sw",              "top",
    "w",              0
  };
  static const unsigned char align_maps[] = {
    QRF_ALIGN_Auto,   QRF_ALIGN_Bottom,  QRF_ALIGN_C,
    QRF_ALIGN_Center, QRF_ALIGN_E,       QRF_ALIGN_Left,
    QRF_ALIGN_Middle, QRF_ALIGN_N,       QRF_ALIGN_NE,
    QRF_ALIGN_NW,     QRF_ALIGN_Right,   QRF_ALIGN_S,
    QRF_ALIGN_SE,     QRF_ALIGN_SW,      QRF_ALIGN_Top,
    QRF_ALIGN_W
  };

  memset(&qrf, 0, sizeof(qrf));
  qrf.i_version = 1;
  qrf.output = &result;
  for(i=2; i<objc; i++){
    const char *arg = Tcl_GetString(objv[i]);
    const char *bools[] = { "auto", "yes", "no", "on", "off", 0 };
    const unsigned char bool_maps[] = { 0, 2, 1, 2, 1 };
    if( arg[0]!='-' ){
      if( sql ){
        Tcl_AppendResult(db->interp, "unknown argument: ", arg, (char*)0);
        rc = TCL_ERROR;
        goto format_failed;
      }
      sql  = arg;
    }else if( i==objc-1 ){
      Tcl_AppendResult(db->interp, "option has no argument: ", arg, (char*)0);
      rc = TCL_ERROR;
      goto format_failed;
    }else if( strcmp(arg,"-style")==0 ){
      static const char *styles[] = {
        "auto",             "box",              "column",
        "count",            "csv",              "eqp",
        "explain",          "html",             "insert",
        "jobject",          "json",             "line",
        "list",             "markdown",         "quote",
        "stats",            "stats-est",        "stats-vm",
        "table",            0
      };
      static unsigned char style_maps[] = {
        QRF_STYLE_Auto,     QRF_STYLE_Box,      QRF_STYLE_Column,
        QRF_STYLE_Count,    QRF_STYLE_Csv,      QRF_STYLE_Eqp,
        QRF_STYLE_Explain,  QRF_STYLE_Html,     QRF_STYLE_Insert,
        QRF_STYLE_JObject,  QRF_STYLE_Json,     QRF_STYLE_Line,
        QRF_STYLE_List,     QRF_STYLE_Markdown, QRF_STYLE_Quote,
        QRF_STYLE_Stats,    QRF_STYLE_StatsEst, QRF_STYLE_StatsVm,
        QRF_STYLE_Table
      };
      int style;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], styles,
                              "format style (-style)", 0, &style);
      if( rc ) goto format_failed;
      qrf.style = style_maps[style];
      i++;
    }else if( strcmp(arg,"-esc")==0 ){
      static const char *escs[] = {
        "ascii",        "auto",         "off",      "symbol",   0
      };
      static unsigned char esc_maps[] = {
        QRF_ESC_Ascii,  QRF_ESC_Auto,   QRF_ESC_Off, QRF_ESC_Symbol
      };
      int esc;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], escs,
                              "control character escape (-esc)", 0, &esc);
      if( rc ) goto format_failed;
      qrf.esc = esc_maps[esc];
      i++;
    }else if( strcmp(arg,"-text")==0 || strcmp(arg, "-title")==0 ){
      /* NB: --title can be "off" or "on but --text may not be.  Thus we put
      ** the "off" and "on" choices first and start the search on the
      ** thrid element of the array when processing --text */
      static const char *texts[] = {           "off",   "on",
        "auto",             "csv",              "html",
        "json",             "plain",            "relaxed",
        "sql",              "tcl",              0
      };
      static unsigned char text_maps[] = {
        QRF_TEXT_Auto,      QRF_TEXT_Csv,       QRF_TEXT_Html,
        QRF_TEXT_Json,      QRF_TEXT_Plain,     QRF_TEXT_Relaxed,
        QRF_TEXT_Sql,       QRF_TEXT_Tcl
      };
      int txt;
      int k = arg[2]=='e';
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], &texts[k*2], arg,
                               0, &txt);
      if( rc ) goto format_failed;
      if( k ){
        qrf.text = text_maps[txt];
      }else if( txt<=1 ){
        qrf.titles = txt ? QRF_Yes : QRF_No;
        qrf.title = QRF_TEXT_Auto;
      }else{
        qrf.titles = QRF_Yes;
        qrf.title = text_maps[txt-2];
      }
      i++;
    }else if( strcmp(arg,"-blob")==0 ){
      static const char *blobs[] = {
        "auto",             "hex",              "json",
        "tcl",              "text",             "sql",
        "size",             0
      };
      static unsigned char blob_maps[] = {
        QRF_BLOB_Auto,      QRF_BLOB_Hex,       QRF_BLOB_Json,
        QRF_BLOB_Tcl,       QRF_BLOB_Text,      QRF_BLOB_Sql,
        QRF_BLOB_Size
      };
      int blob;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], blobs,
                              "BLOB encoding (-blob)", 0, &blob);
      if( rc ) goto format_failed;
      qrf.blob = blob_maps[blob];
      i++;
    }else if( strcmp(arg,"-wordwrap")==0 ){
      int v = 0;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], bools,
                              "-wordwrap", 0, &v);
      if( rc ) goto format_failed;
      qrf.word_wrap = bool_maps[v];
      i++;
    }else if( strcmp(arg,"-textjsonb")==0
           || strcmp(arg,"-splitcolumn")==0
           || strcmp(arg,"-border")==0
    ){
      int v = 0;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], bools,
                              arg, 0, &v);
      if( rc ) goto format_failed;
      if( arg[1]=='t' ){
        qrf.text_jsonb = bool_maps[v];
      }else if( arg[1]=='b' ){
        qrf.border = bool_maps[v];
      }else{
        qrf.split_column = bool_maps[v];
      }
      i++;
    }else if( strcmp(arg,"-defaultalign")==0 || strcmp(arg,"-titlealign")==0){
      int ax = 0;
      rc = Tcl_GetIndexFromObj(db->interp, objv[i+1], aligns,
                    arg[1]=='d' ?  "default alignment (-defaultalign)" :
                                    "title alignment (-titlealign)",
                    0, &ax);
      if( rc ) goto format_failed;
      if( arg[1]=='d' ){
        qrf.dflt_align = align_maps[ax];
      }else{
        qrf.title_align = align_maps[ax];
      }
      i++;
    }else if( strcmp(arg,"-wrap")==0
           || strcmp(arg,"-screenwidth")==0 
           || strcmp(arg,"-linelimit")==0 
           || strcmp(arg,"-titlelimit")==0 
    ){
      int v = 0;
      rc = Tcl_GetIntFromObj(db->interp, objv[i+1], &v);
      if( rc ) goto format_failed;
      if( v<QRF_MIN_WIDTH ){
        v = QRF_MIN_WIDTH;
      }else if( v>QRF_MAX_WIDTH ){
        v = QRF_MAX_WIDTH;
      }
      if( arg[1]=='w' ){
        qrf.n_wrap = v;
      }else if( arg[1]=='s' ){
        qrf.n_screen_width = v;
      }else if( arg[1]=='t' ){
        qrf.n_title_limit = v;
      }else{
        qrf.n_line_limit = v;
      }
      i++;
    }else if( strcmp(arg,"-charlimit")==0 ){
      int v = 0;
      rc = Tcl_GetIntFromObj(db->interp, objv[i+1], &v);
      if( rc ) goto format_failed;
      if( v<0 ) v = 0;
      qrf.n_char_limit = v;
      i++;
    }else if( strcmp(arg,"-multiinsert")==0 ){
      int v = 0;
      rc = Tcl_GetIntFromObj(db->interp, objv[i+1], &v);
      if( rc ) goto format_failed;
      if( v<0 ) v = 0;
      qrf.n_multi_insert = v;
      i++;
    }else if( strcmp(arg,"-align")==0 ){
      TCL_SIZE n = 0;
      int jj;
      rc = Tcl_ListObjLength(db->interp, objv[i+1], &n);
      if( rc ) goto format_failed;
      dbsql_free(qrf.aligns);
      qrf.aligns = dbsql_malloc64( (n+1)*sizeof(qrf.aligns[0]) );
      if( qrf.aligns==0 ){
        Tcl_AppendResult(db->interp, "out of memory", (char*)0);
        rc = TCL_ERROR;
        goto format_failed;
      }
      memset(qrf.aligns, 0, (n+1)*sizeof(qrf.aligns[0]));
      qrf.n_align = n;
      for(jj=0; jj<n; jj++){
        int x;
        Tcl_Obj *term;
        rc = Tcl_ListObjIndex(db->interp, objv[i+1], jj, &term);
        if( rc ) goto format_failed;
        rc = Tcl_GetIndexFromObj(db->interp, term, aligns,
                          "column alignment (-align)", 0, &x);
        if( rc ) goto format_failed;
        qrf.aligns[jj] = align_maps[x];
      }
      i++;
    }else if( strcmp(arg,"-widths")==0 ){
      TCL_SIZE n = 0;
      int jj;
      rc = Tcl_ListObjLength(db->interp, objv[i+1], &n);
      if( rc ) goto format_failed;
      dbsql_free(qrf.widths);
      qrf.widths = dbsql_malloc64( (n+1)*sizeof(qrf.widths[0]) );
      if( qrf.widths==0 ){
        Tcl_AppendResult(db->interp, "out of memory", (char*)0);
        rc = TCL_ERROR;
        goto format_failed;
      }
      memset(qrf.widths, 0, (n+1)*sizeof(qrf.widths[0]));
      qrf.n_width = n;
      for(jj=0; jj<n; jj++){
        Tcl_Obj *term;
        int v;
        rc = Tcl_ListObjIndex(db->interp, objv[i+1], jj, &term);
        if( rc ) goto format_failed;
        rc = Tcl_GetIntFromObj(db->interp, term, &v);
        if( v<(-QRF_MAX_WIDTH) ){
          v = -QRF_MAX_WIDTH;
        }else if( v>QRF_MAX_WIDTH ){
          v = QRF_MAX_WIDTH;
        }
        qrf.widths[jj] = (short int)v;
      }
      i++;
    }else if( strcmp(arg,"-columnsep")==0 ){
      qrf.column_sep = Tcl_GetString(objv[i+1]);
      i++;
    }else if( strcmp(arg,"-rowsep")==0 ){
      qrf.row_sep = Tcl_GetString(objv[i+1]);
      i++;
    }else if( strcmp(arg,"-tablename")==0 ){
      qrf.table_name = Tcl_GetString(objv[i+1]);
      i++;
    }else if( strcmp(arg,"-null")==0 ){
      qrf.null = Tcl_GetString(objv[i+1]);
      i++;
    }else if( strcmp(arg,"-version")==0 ){
      /* Undocumented. Testing use only */
      qrf.i_version = atoi(Tcl_GetString(objv[i+1]));
      i++;
    }else{
      Tcl_AppendResult(db->interp, "unknown option: ", arg, (char*)0);
      rc = TCL_ERROR;
      goto format_failed;
    }
  }
  while( sql && sql[0] ){
    sql_prepared_stmt_t *stmt = 0;        /* Next statement to run */
    char *err = 0;                    /* Error message from QRF */

    rc = __db_prepare_and_bind(db, sql, &sql, &stmt);
    if( rc ) goto format_failed;
    if( stmt==0 ) continue;
    rc = dbsql_format_query_result(stmt->stmt, &qrf, &err);
    __db_release_stmt(db, stmt, 0);
    if( rc ){
      Tcl_SetResult(db->interp, err, TCL_VOLATILE);
      dbsql_free(err);
      rc = TCL_ERROR;
      goto format_failed;
    }
  }
  Tcl_SetResult(db->interp, result, TCL_VOLATILE);
  rc = TCL_OK;
  /* Fall through...*/
  
format_failed:
  dbsql_free(qrf.widths);
  dbsql_free(qrf.aligns);
  dbsql_free(result);
  return rc;

#endif
}

/*
** The "sqlite" command below creates a new Tcl command for each
** connection it opens to an dbsql database.  This routine is invoked
** whenever one of those connection-specific commands is executed
** in Tcl.  For example, if you run Tcl code like this:
**
**       DBSQL db1  "my_database"
**       db1 close
**
** The first command opens a connection to the "my_database" database
** and calls that connection "db1".  The second command causes this
** subroutine to be invoked.
*/
static int DBSQL_TCLAPI __db_obj_cmd(
  void *cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const*objv
){
  dbsql_db *db = (dbsql_db*)cd;
  int choice;
  int rc = TCL_OK;
  static const char *DB_strs[] = {
    "authorizer",             "backup",                "bind_fallback",
    "busy",                   "cache",                 "changes",
    "close",                  "collate",               "collation_needed",
    "commit_hook",            "complete",              "config",
    "copy",                   "deserialize",           "enable_load_extension",
    "errorcode",              "erroroffset",           "eval",
    "exists",                 "format",                "function",
    "incrblob",               "interrupt",             "last_insert_rowid",
    "nullvalue",              "onecolumn",             "preupdate",
    "profile",                "progress",              "rekey",
    "restore",                "rollback_hook",         "serialize",
    "status",                 "timeout",               "total_changes",
    "trace",                  "trace_v2",              "transaction",
    "unlock_notify",          "update_hook",           "version",
    "wal_hook",               0                        
  };
  enum DB_enum {
    DB_AUTHORIZER,            DB_BACKUP,               DB_BIND_FALLBACK,
    DB_BUSY,                  DB_CACHE,                DB_CHANGES,
    DB_CLOSE,                 DB_COLLATE,              DB_COLLATION_NEEDED,
    DB_COMMIT_HOOK,           DB_COMPLETE,             DB_CONFIG,
    DB_COPY,                  DB_DESERIALIZE,          DB_ENABLE_LOAD_EXTENSION,
    DB_ERRORCODE,             DB_ERROROFFSET,          DB_EVAL,
    DB_EXISTS,                DB_FORMAT,               DB_FUNCTION,
    DB_INCRBLOB,              DB_INTERRUPT,            DB_LAST_INSERT_ROWID,
    DB_NULLVALUE,             DB_ONECOLUMN,            DB_PREUPDATE,
    DB_PROFILE,               DB_PROGRESS,             DB_REKEY,
    DB_RESTORE,               DB_ROLLBACK_HOOK,        DB_SERIALIZE,
    DB_STATUS,                DB_TIMEOUT,              DB_TOTAL_CHANGES,
    DB_TRACE,                 DB_TRACE_V2,             DB_TRANSACTION,
    DB_UNLOCK_NOTIFY,         DB_UPDATE_HOOK,          DB_VERSION,
    DB_WAL_HOOK
  };
  /* don't leave trailing commas on DB_enum, it confuses the AIX xlc compiler */

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], DB_strs, "option", 0, &choice) ){
    return TCL_ERROR;
  }

  switch( (enum DB_enum)choice ){

  /*    $db authorizer ?CALLBACK?
  **
  ** Invoke the given callback to authorize each SQL operation as it is
  ** compiled.  5 arguments are appended to the callback before it is
  ** invoked:
  **
  **   (1) The authorization type (ex: DBSQL_CREATE_TABLE, DBSQL_INSERT, ...)
  **   (2) First descriptive name (depends on authorization type)
  **   (3) Second descriptive name
  **   (4) Name of the database (ex: "main", "temp")
  **   (5) Name of trigger that is doing the access
  **
  ** The callback should return one of the following strings: DBSQL_OK,
  ** DBSQL_IGNORE, or DBSQL_DENY.  Any other return value is an error.
  **
  ** If this method is invoked with no arguments, the current authorization
  ** callback string is returned.
  */
  case DB_AUTHORIZER: {
#ifdef DBSQL_OMIT_AUTHORIZATION
    Tcl_AppendResult(interp, "authorization not available in this build",
                     (char*)0);
    return TCL_ERROR;
#else
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->auth ){
        Tcl_AppendResult(interp, db->auth, (char*)0);
      }
    }else{
      char *auth;
      TCL_SIZE len;
      if( db->auth ){
        Tcl_Free(db->auth);
      }
      auth = Tcl_GetStringFromObj(objv[2], &len);
      if( auth && len>0 ){
        db->auth = Tcl_Alloc( len + 1 );
        memcpy(db->auth, auth, len+1);
      }else{
        db->auth = 0;
      }
      if( db->auth ){
        typedef int (*dbsql_auth_cb)(
           void*,int,const char*,const char*,
           const char*,const char*);
        db->interp = interp;
        dbsql_set_authorizer(db->db,(dbsql_auth_cb)auth_callback,db);
      }else{
        dbsql_set_authorizer(db->db, 0, 0);
      }
    }
#endif
    break;
  }

  /*    $db backup ?DATABASE? FILENAME
  **
  ** Open or create a database file named FILENAME.  Transfer the
  ** content of local database DATABASE (default: "main") into the
  ** FILENAME database.
  */
  case DB_BACKUP: {
    const char *dest_file;
    const char *src_db;
    DBSQL *dest;
    dbsql_backup_t *backup;

    if( objc==3 ){
      src_db = "main";
      dest_file = Tcl_GetString(objv[2]);
    }else if( objc==4 ){
      src_db = Tcl_GetString(objv[2]);
      dest_file = Tcl_GetString(objv[3]);
    }else{
      Tcl_WrongNumArgs(interp, 2, objv, "?DATABASE? FILENAME");
      return TCL_ERROR;
    }
    rc = dbsql_open_v2(dest_file, &dest,
               DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE| db->open_flags, 0);
    if( rc!=DBSQL_OK ){
      Tcl_AppendResult(interp, "cannot open target database: ",
           dbsql_errmsg(dest), (char*)0);
      dbsql_close(dest);
      return TCL_ERROR;
    }
    backup = dbsql_backup_init(dest, "main", db->db, src_db);
    if( backup==0 ){
      Tcl_AppendResult(interp, "backup failed: ",
           dbsql_errmsg(dest), (char*)0);
      dbsql_close(dest);
      return TCL_ERROR;
    }
    while(  (rc = dbsql_backup_step(backup,100))==DBSQL_OK ){}
    dbsql_backup_finish(backup);
    if( rc==DBSQL_DONE ){
      rc = TCL_OK;
    }else{
      Tcl_AppendResult(interp, "backup failed: ",
           dbsql_errmsg(dest), (char*)0);
      rc = TCL_ERROR;
    }
    dbsql_close(dest);
    break;
  }

  /*    $db bind_fallback ?CALLBACK?
  **
  ** When resolving bind parameters in an SQL statement, if the parameter
  ** cannot be associated with a TCL variable then invoke CALLBACK with a
  ** single argument that is the name of the parameter and use the return
  ** value of the CALLBACK as the binding.  If CALLBACK returns something
  ** other than TCL_OK or TCL_ERROR then bind a NULL.
  **
  ** If CALLBACK is an empty string, then revert to the default behavior 
  ** which is to set the binding to NULL.
  **
  ** If CALLBACK returns an error, that causes the statement execution to
  ** abort.  Hence, to configure a connection so that it throws an error
  ** on an attempt to bind an unknown variable, do something like this:
  **
  **     proc bind_error {name} {error "no such variable: $name"}
  **     db bind_fallback bind_error
  */
  case DB_BIND_FALLBACK: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->bind_fallback ){
        Tcl_AppendResult(interp, db->bind_fallback, (char*)0);
      }
    }else{
      char *callback;
      TCL_SIZE len;
      if( db->bind_fallback ){
        Tcl_Free(db->bind_fallback);
      }
      callback = Tcl_GetStringFromObj(objv[2], &len);
      if( callback && len>0 ){
        db->bind_fallback = Tcl_Alloc( len + 1 );
        memcpy(db->bind_fallback, callback, len+1);
      }else{
        db->bind_fallback = 0;
      }
    }
    break;
  }

  /*    $db busy ?CALLBACK?
  **
  ** Invoke the given callback if an SQL statement attempts to open
  ** a locked database file.
  */
  case DB_BUSY: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "CALLBACK");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->busy ){
        Tcl_AppendResult(interp, db->busy, (char*)0);
      }
    }else{
      char *busy;
      TCL_SIZE len;
      if( db->busy ){
        Tcl_Free(db->busy);
      }
      busy = Tcl_GetStringFromObj(objv[2], &len);
      if( busy && len>0 ){
        db->busy = Tcl_Alloc( len + 1 );
        memcpy(db->busy, busy, len+1);
      }else{
        db->busy = 0;
      }
      if( db->busy ){
        db->interp = interp;
        dbsql_busy_handler(db->db, __db_busy_handler, db);
      }else{
        dbsql_busy_handler(db->db, 0, 0);
      }
    }
    break;
  }

  /*     $db cache flush
  **     $db cache size n
  **
  ** Flush the prepared statement cache, or set the maximum number of
  ** cached statements.
  */
  case DB_CACHE: {
    char *sub_cmd;
    int n;

    if( objc<=2 ){
      Tcl_WrongNumArgs(interp, 1, objv, "cache option ?arg?");
      return TCL_ERROR;
    }
    sub_cmd = Tcl_GetStringFromObj( objv[2], 0 );
    if( *sub_cmd=='f' && strcmp(sub_cmd,"flush")==0 ){
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "flush");
        return TCL_ERROR;
      }else{
        __flush_stmt_cache( db );
      }
    }else if( *sub_cmd=='s' && strcmp(sub_cmd,"size")==0 ){
      if( objc!=4 ){
        Tcl_WrongNumArgs(interp, 2, objv, "size n");
        return TCL_ERROR;
      }else{
        if( TCL_ERROR==Tcl_GetIntFromObj(interp, objv[3], &n) ){
          Tcl_AppendResult( interp, "cannot convert \"",
               Tcl_GetStringFromObj(objv[3],0), "\" to integer", (char*)0);
          return TCL_ERROR;
        }else{
          if( n<0 ){
            __flush_stmt_cache( db );
            n = 0;
          }else if( n>MAX_PREPARED_STMTS ){
            n = MAX_PREPARED_STMTS;
          }
          db->max_stmt = n;
        }
      }
    }else{
      Tcl_AppendResult( interp, "bad option \"",
          Tcl_GetStringFromObj(objv[2],0), "\": must be flush or size",
          (char*)0);
      return TCL_ERROR;
    }
    break;
  }

  /*     $db changes
  **
  ** Return the number of rows that were modified, inserted, or deleted by
  ** the most recent INSERT, UPDATE or DELETE statement, not including
  ** any changes made by trigger programs.
  */
  case DB_CHANGES: {
    Tcl_Obj *result;
    if( objc!=2 ){
      Tcl_WrongNumArgs(interp, 2, objv, "");
      return TCL_ERROR;
    }
    result = Tcl_GetObjResult(interp);
    Tcl_SetWideIntObj(result, dbsql_changes64(db->db));
    break;
  }

  /*    $db close
  **
  ** Shutdown the database
  */
  case DB_CLOSE: {
    Tcl_DeleteCommand(interp, Tcl_GetStringFromObj(objv[0], 0));
    break;
  }

  /*
  **     $db collate NAME SCRIPT
  **
  ** Create a new SQL collation function called NAME.  Whenever
  ** that function is called, invoke SCRIPT to evaluate the function.
  */
  case DB_COLLATE: {
    sql_collate_t *collate;
    char *name;
    char *z_script;
    TCL_SIZE n_script;
    if( objc!=4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NAME SCRIPT");
      return TCL_ERROR;
    }
    name = Tcl_GetStringFromObj(objv[2], 0);
    z_script = Tcl_GetStringFromObj(objv[3], &n_script);
    collate = (sql_collate_t*)Tcl_Alloc( sizeof(*collate) + n_script + 1 );
    if( collate==0 ) return TCL_ERROR;
    collate->interp = interp;
    collate->next = db->collate;
    collate->script = (char*)&collate[1];
    db->collate = collate;
    memcpy(collate->script, z_script, n_script+1);
    if( dbsql_create_collation(db->db, name, DBSQL_UTF8,
        collate, __tcl_sql_collate) ){
      Tcl_SetResult(interp, (char *)dbsql_errmsg(db->db), TCL_VOLATILE);
      return TCL_ERROR;
    }
    break;
  }

  /*
  **     $db collation_needed SCRIPT
  **
  ** Create a new SQL collation function called NAME.  Whenever
  ** that function is called, invoke SCRIPT to evaluate the function.
  */
  case DB_COLLATION_NEEDED: {
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SCRIPT");
      return TCL_ERROR;
    }
    if( db->collate_needed ){
      Tcl_DecrRefCount(db->collate_needed);
    }
    db->collate_needed = Tcl_DuplicateObj(objv[2]);
    Tcl_IncrRefCount(db->collate_needed);
    dbsql_collation_needed(db->db, db, __tcl_collate_needed);
    break;
  }

  /*    $db commit_hook ?CALLBACK?
  **
  ** Invoke the given callback just before committing every SQL transaction.
  ** If the callback throws an exception or returns non-zero, then the
  ** transaction is aborted.  If CALLBACK is an empty string, the callback
  ** is disabled.
  */
  case DB_COMMIT_HOOK: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->commit ){
        Tcl_AppendResult(interp, db->commit, (char*)0);
      }
    }else{
      const char *commit;
      TCL_SIZE len;
      if( db->commit ){
        Tcl_Free(db->commit);
      }
      commit = Tcl_GetStringFromObj(objv[2], &len);
      if( commit && len>0 ){
        db->commit = Tcl_Alloc( len + 1 );
        memcpy(db->commit, commit, len+1);
      }else{
        db->commit = 0;
      }
      if( db->commit ){
        db->interp = interp;
        dbsql_commit_hook(db->db, __db_commit_handler, db);
      }else{
        dbsql_commit_hook(db->db, 0, 0);
      }
    }
    break;
  }

  /*    $db complete SQL
  **
  ** Return TRUE if SQL is a complete SQL statement.  Return FALSE if
  ** additional lines of input are needed.  This is similar to the
  ** built-in "info complete" command of Tcl.
  */
  case DB_COMPLETE: {
#ifndef DBSQL_OMIT_COMPLETE
    Tcl_Obj *result;
    int is_complete;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SQL");
      return TCL_ERROR;
    }
    is_complete = dbsql_complete( Tcl_GetStringFromObj(objv[2], 0) );
    result = Tcl_GetObjResult(interp);
    Tcl_SetBooleanObj(result, is_complete);
#endif
    break;
  }

  /*    $db config ?OPTION? ?BOOLEAN?
  **
  ** Configure the database connection using the dbsql_db_config()
  ** interface.
  */
  case DB_CONFIG: {
    static const struct db_config_choices_t {
      const char *name;
      int op;
    } db_configs[] = {
        { "defensive",          DBSQL_DBCONFIG_DEFENSIVE             },
        { "dqs_ddl",            DBSQL_DBCONFIG_DQS_DDL               },
        { "dqs_dml",            DBSQL_DBCONFIG_DQS_DML               },
        { "enable_fkey",        DBSQL_DBCONFIG_ENABLE_FKEY           },
        { "enable_qpsg",        DBSQL_DBCONFIG_ENABLE_QPSG           },
        { "enable_trigger",     DBSQL_DBCONFIG_ENABLE_TRIGGER        },
        { "enable_view",        DBSQL_DBCONFIG_ENABLE_VIEW           },
        { "fts3_tokenizer",     DBSQL_DBCONFIG_ENABLE_FTS3_TOKENIZER },
        { "legacy_alter_table", DBSQL_DBCONFIG_LEGACY_ALTER_TABLE    },
        { "legacy_file_format", DBSQL_DBCONFIG_LEGACY_FILE_FORMAT    },
        { "load_extension",     DBSQL_DBCONFIG_ENABLE_LOAD_EXTENSION },
        { "no_ckpt_on_close",   DBSQL_DBCONFIG_NO_CKPT_ON_CLOSE      },
        { "reset_database",     DBSQL_DBCONFIG_RESET_DATABASE        },
        { "trigger_eqp",        DBSQL_DBCONFIG_TRIGGER_EQP           },
        { "trusted_schema",     DBSQL_DBCONFIG_TRUSTED_SCHEMA        },
        { "writable_schema",    DBSQL_DBCONFIG_WRITABLE_SCHEMA       },
    };
    Tcl_Obj *result;
    int ii;
    if( objc>4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?OPTION? ?BOOLEAN?");
      return TCL_ERROR;
    }
    if( objc==2 ){
      /* with_t no arguments, list all configuration options and with the
      ** current value */
      result = Tcl_NewListObj(0,0);
      for(ii=0; ii<sizeof(db_configs)/sizeof(db_configs[0]); ii++){
        int v = 0;
        dbsql_db_config(db->db, db_configs[ii].op, -1, &v);
        Tcl_ListObjAppendElement(interp, result,
           Tcl_NewStringObj(db_configs[ii].name,-1));
        Tcl_ListObjAppendElement(interp, result,
           Tcl_NewIntObj(v));
      }
    }else{
      const char *opt = Tcl_GetString(objv[2]);
      int onoff = -1;
      int v = 0;
      if( opt[0]=='-' ) opt++;
      for(ii=0; ii<sizeof(db_configs)/sizeof(db_configs[0]); ii++){
        if( strcmp(db_configs[ii].name, opt)==0 ) break;
      }
      if( ii>=sizeof(db_configs)/sizeof(db_configs[0]) ){
        Tcl_AppendResult(interp, "unknown config option: \"", opt,
                                "\"", (void*)0);
        return TCL_ERROR;
      }
      if( objc==4 ){
        if( Tcl_GetBooleanFromObj(interp, objv[3], &onoff) ){
          return TCL_ERROR;
        }
      }
      dbsql_db_config(db->db, db_configs[ii].op, onoff, &v);
      result = Tcl_NewIntObj(v);
    }
    Tcl_SetObjResult(interp, result);
    break;
  }

  /*    $db copy conflict-algorithm table filename ?SEPARATOR? ?NULLINDICATOR?
  **
  ** Copy data into table from filename, optionally using SEPARATOR
  ** as column separators.  If a column contains a null string, or the
  ** value of NULLINDICATOR, a NULL is inserted for the column.
  ** conflict-algorithm is one of the sqlite conflict algorithms:
  **    rollback, abort, fail, ignore, replace
  ** On success, return the number of lines processed, not necessarily same
  ** as 'db changes' due to conflict-algorithm selected.
  **
  ** This code is basically an implementation/enhancement of
  ** the DBSQL shell.c ".import" command.
  **
  ** This command usage is equivalent to the sqlite2.x COPY statement,
  ** which imports file data into a table using the __postgre_sql COPY file format:
  **   $db copy $conflict_algorithm $table_name $filename \t \\N
  */
  case DB_COPY: {
    char *table;               /* Insert data into this table */
    char *file;                /* The file from which to extract data */
    char *conflict;            /* The conflict algorithm to use */
    dbsql_stmt_t *stmt;        /* A statement */
    int n_col;                   /* Number of columns in the table */
    int n_byte;                  /* Number of bytes in an SQL string */
    int i, j;                   /* Loop counters */
    int n_sep;                   /* Number of bytes in sep[] */
    int n_null;                  /* Number of bytes in null[] */
    char *sql;                 /* An SQL statement */
    char *line;                /* A single line of input from the file */
    char **cols;               /* line[] broken up into columns */
    const char *commit;        /* How to commit changes */
    Tcl_Channel in;             /* The input file */
    int lineno = 0;             /* Line number of input file */
    char line_num[80];          /* Line number print buffer */
    Tcl_Obj *str;
    Tcl_Obj *result;           /* interp result */

    const char *sep;
    const char *null;
    if( objc<5 || objc>7 ){
      Tcl_WrongNumArgs(interp, 2, objv,
         "CONFLICT-ALGORITHM TABLE FILENAME ?SEPARATOR? ?NULLINDICATOR?");
      return TCL_ERROR;
    }
    if( objc>=6 ){
      sep = Tcl_GetStringFromObj(objv[5], 0);
    }else{
      sep = "\t";
    }
    if( objc>=7 ){
      null = Tcl_GetStringFromObj(objv[6], 0);
    }else{
      null = "";
    }
    conflict = Tcl_GetStringFromObj(objv[2], 0);
    table = Tcl_GetStringFromObj(objv[3], 0);
    file = Tcl_GetStringFromObj(objv[4], 0);
    n_sep = strlen30(sep);
    n_null = strlen30(null);
    if( n_sep==0 ){
      Tcl_AppendResult(interp,"Error: non-null separator required for copy",
                       (char*)0);
      return TCL_ERROR;
    }
    if(strcmp(conflict, "rollback") != 0 &&
       strcmp(conflict, "abort"   ) != 0 &&
       strcmp(conflict, "fail"    ) != 0 &&
       strcmp(conflict, "ignore"  ) != 0 &&
       strcmp(conflict, "replace" ) != 0 ) {
      Tcl_AppendResult(interp, "Error: \"", conflict,
            "\", conflict-algorithm must be one of: rollback, "
            "abort, fail, ignore, or replace", (char*)0);
      return TCL_ERROR;
    }
    sql = dbsql_mprintf("SELECT * FROM '%q'", table);
    if( sql==0 ){
      Tcl_AppendResult(interp, "Error: no such table: ", table, (char*)0);
      return TCL_ERROR;
    }
    n_byte = strlen30(sql);
    rc = dbsql_prepare(db->db, sql, -1, &stmt, 0);
    dbsql_free(sql);
    if( rc ){
      Tcl_AppendResult(interp, "Error: ", dbsql_errmsg(db->db), (char*)0);
      n_col = 0;
    }else{
      n_col = dbsql_column_count(stmt);
    }
    dbsql_finalize(stmt);
    if( n_col==0 ) {
      return TCL_ERROR;
    }
    sql = malloc( n_byte + 50 + n_col*2 );
    if( sql==0 ) {
      Tcl_AppendResult(interp, "Error: can't malloc()", (char*)0);
      return TCL_ERROR;
    }
    dbsql_snprintf(n_byte+50, sql, "INSERT OR %q INTO '%q' VALUES(?",
         conflict, table);
    j = strlen30(sql);
    for(i=1; i<n_col; i++){
      sql[j++] = ',';
      sql[j++] = '?';
    }
    sql[j++] = ')';
    sql[j] = 0;
    rc = dbsql_prepare(db->db, sql, -1, &stmt, 0);
    free(sql);
    if( rc ){
      Tcl_AppendResult(interp, "Error: ", dbsql_errmsg(db->db), (char*)0);
      dbsql_finalize(stmt);
      return TCL_ERROR;
    }
    in = Tcl_OpenFileChannel(interp, file, "rb", 0666);
    if( in==0 ){
      dbsql_finalize(stmt);
      return TCL_ERROR;
    }
    Tcl_SetChannelOption(NULL, in, "-translation", "auto");
    cols = malloc( sizeof(cols[0])*(n_col+1) );
    if( cols==0 ) {
      Tcl_AppendResult(interp, "Error: can't malloc()", (char*)0);
      Tcl_Close(interp, in);
      return TCL_ERROR;
    }
    str = Tcl_NewObj();
    Tcl_IncrRefCount(str);
    (void)dbsql_exec(db->db, "BEGIN", 0, 0, 0);
    commit = "COMMIT";
    while( Tcl_GetsObj(in, str)>=0 ) {
      char *z;
      TCL_SIZE byte_len;
      lineno++;
      line = (char *)Tcl_GetByteArrayFromObj(str, &byte_len);
      cols[0] = line;
      for(i=0, z=line; *z; z++){
        if( *z==sep[0] && strncmp(z, sep, n_sep)==0 ){
          *z = 0;
          i++;
          if( i<n_col ){
            cols[i] = &z[n_sep];
            z += n_sep-1;
          }
        }
      }
      if( i+1!=n_col ){
        char *err;
        int n_err = strlen30(file) + 200;
        err = malloc(n_err);
        if( err ){
          dbsql_snprintf(n_err, err,
             "Error: %s line %d: expected %d columns of data but found %d",
             file, lineno, n_col, i+1);
          Tcl_AppendResult(interp, err, (char*)0);
          free(err);
        }
        commit = "ROLLBACK";
        break;
      }
      for(i=0; i<n_col; i++){
        /* check for null data, if so, bind as null */
        if( (n_null>0 && strcmp(cols[i], null)==0)
          || strlen30(cols[i])==0
        ){
          dbsql_bind_null(stmt, i+1);
        }else{
          dbsql_bind_text(stmt, i+1, cols[i], -1, DBSQL_STATIC);
        }
      }
      dbsql_step(stmt);
      rc = dbsql_reset(stmt);
      Tcl_SetObjLength(str, 0);
      if( rc!=DBSQL_OK ){
        Tcl_AppendResult(interp,"Error: ", dbsql_errmsg(db->db), (char*)0);
        commit = "ROLLBACK";
        break;
      }
    }
    Tcl_DecrRefCount(str);
    free(cols);
    Tcl_Close(interp, in);
    dbsql_finalize(stmt);
    (void)dbsql_exec(db->db, commit, 0, 0, 0);

    if( commit[0] == 'C' ){
      /* success, set result as number of lines processed */
      result = Tcl_GetObjResult(interp);
      Tcl_SetIntObj(result, lineno);
      rc = TCL_OK;
    }else{
      /* failure, append lineno where failed */
      dbsql_snprintf(sizeof(line_num), line_num,"%d",lineno);
      Tcl_AppendResult(interp,", failed while processing line: ",line_num,
                       (char*)0);
      rc = TCL_ERROR;
    }
    break;
  }

  /*
  **     $db deserialize ?-maxsize N? ?-readonly BOOL? ?DATABASE? VALUE
  **
  ** Reopen DATABASE (default "main") using the content in $VALUE
  */
  case DB_DESERIALIZE: {
#ifdef DBSQL_OMIT_DESERIALIZE
    Tcl_AppendResult(interp, "MEMDB not available in this build",
                     (char*)0);
    rc = TCL_ERROR;
#else
    const char *schema = 0;
    Tcl_Obj *value = 0;
    unsigned char *ba;
    unsigned char *data;
    TCL_SIZE len;
    int xrc;
    int64_t mx_size = 0;
    int i;
    int is_readonly = 0;


    if( objc<3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?DATABASE? VALUE");
      rc = TCL_ERROR;
      break;
    }
    for(i=2; i<objc-1; i++){
      const char *z = Tcl_GetString(objv[i]);
      if( strcmp(z,"-maxsize")==0 && i<objc-2 ){
        Tcl_WideInt x;
        rc = Tcl_GetWideIntFromObj(interp, objv[++i], &x);
        if( rc ) goto deserialize_error;
        mx_size = x;
        continue;
      }
      if( strcmp(z,"-readonly")==0 && i<objc-2 ){
        rc = Tcl_GetBooleanFromObj(interp, objv[++i], &is_readonly);
        if( rc ) goto deserialize_error;
        continue;
      }
      if( schema==0 && i==objc-2 && z[0]!='-' ){
        schema = z;
        continue;
      }
      Tcl_AppendResult(interp, "unknown option: ", z, (char*)0);
      rc = TCL_ERROR;
      goto deserialize_error;
    }
    value = objv[objc-1];
    ba = Tcl_GetByteArrayFromObj(value, &len);
    data = dbsql_malloc64( len );
    if( data==0 && len>0 ){
      Tcl_AppendResult(interp, "out of memory", (char*)0);
      rc = TCL_ERROR;
    }else{
      int flags;
      if( len>0 ) memcpy(data, ba, len);
      if( is_readonly ){
        flags = DBSQL_DESERIALIZE_FREEONCLOSE | DBSQL_DESERIALIZE_READONLY;
      }else{
        flags = DBSQL_DESERIALIZE_FREEONCLOSE | DBSQL_DESERIALIZE_RESIZEABLE;
      }
      xrc = dbsql_deserialize(db->db, schema, data, len, len, flags);
      if( xrc ){
        Tcl_AppendResult(interp, "unable to set MEMDB content", (char*)0);
        rc = TCL_ERROR;
      }
      if( mx_size>0 ){
        dbsql_file_control(db->db, schema,DBSQL_FCNTL_SIZE_LIMIT,&mx_size);
      }
    }
deserialize_error:
#endif
    break; 
  }

  /*
  **    $db enable_load_extension BOOLEAN
  **
  ** Turn the extension loading feature on or off.  It if off by
  ** default.
  */
  case DB_ENABLE_LOAD_EXTENSION: {
#ifndef DBSQL_OMIT_LOAD_EXTENSION
    int onoff;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "BOOLEAN");
      return TCL_ERROR;
    }
    if( Tcl_GetBooleanFromObj(interp, objv[2], &onoff) ){
      return TCL_ERROR;
    }
    dbsql_enable_load_extension(db->db, onoff);
    break;
#else
    Tcl_AppendResult(interp, "extension loading is turned off at compile-time",
                     (char*)0);
    return TCL_ERROR;
#endif
  }

  /*
  **    $db errorcode
  **
  ** Return the numeric error code that was returned by the most recent
  ** call to dbsql_exec().
  */
  case DB_ERRORCODE: {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(dbsql_errcode(db->db)));
    break;
  }

  /*
  **    $db erroroffset
  **
  ** Return the numeric error code that was returned by the most recent
  ** call to dbsql_exec().
  */
  case DB_ERROROFFSET: {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(dbsql_error_offset(db->db)));
    break;
  }

  /*
  **    $db exists $sql
  **    $db onecolumn $sql
  **
  ** The onecolumn method is the equivalent of:
  **     lindex [$db eval $sql] 0
  */
  case DB_EXISTS:
  case DB_ONECOLUMN: {
    Tcl_Obj *result = 0;
    db_eval_context_t s_eval;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SQL");
      return TCL_ERROR;
    }

    __db_eval_init(&s_eval, db, objv[2], 0, 0);
    rc = __db_eval_step(&s_eval);
    if( choice==DB_ONECOLUMN ){
      if( rc==TCL_OK ){
        result = __db_eval_column_value(&s_eval, 0);
      }else if( rc==TCL_BREAK ){
        Tcl_ResetResult(interp);
      }
    }else if( rc==TCL_BREAK || rc==TCL_OK ){
      result = Tcl_NewBooleanObj(rc==TCL_OK);
    }
    __db_eval_finalize(&s_eval);
    if( result ) Tcl_SetObjResult(interp, result);

    if( rc==TCL_BREAK ){
      rc = TCL_OK;
    }
    break;
  }

  /*
  **    $db eval ?options? $sql ?varName? ?{  ...code... }?
  **
  ** The SQL statement in $sql is evaluated.  For each row, the values
  ** are placed in elements of the array or dict named $varName and
  ** ...code... is executed.  If $varName and $code are omitted, then
  ** no callback is ever invoked.  If $varName is an empty string,
  ** then the values are placed in variables that have the same name
  ** as the fields extracted by the query, and those variables are
  ** accessible during the eval of $code.
  */
  case DB_EVAL: {
    int eval_flags = 0;
    const char *opt;
    while( objc>3 && (opt = Tcl_GetString(objv[2]))!=0 && opt[0]=='-' ){
      if( strcmp(opt, "-withoutnulls")==0 ){
        eval_flags |= DBSQL_EVAL_WITHOUTNULLS;
      }else if( strcmp(opt, "-asdict")==0 ){
        eval_flags |= DBSQL_EVAL_ASDICT;
      }else{
        Tcl_AppendResult(interp, "unknown option: \"", opt, "\"", (void*)0);
        return TCL_ERROR;
      }
      objc--;
      objv++;
    }
    if( objc<3 || objc>5 ){
      Tcl_WrongNumArgs(interp, 2, objv,
          "?OPTIONS? SQL ?VAR-NAME? ?SCRIPT?");
      return TCL_ERROR;
    }

    if( objc==3 ){
      db_eval_context_t s_eval;
      Tcl_Obj *ret = Tcl_NewObj();
      Tcl_IncrRefCount(ret);
      __db_eval_init(&s_eval, db, objv[2], 0, 0);
      while( TCL_OK==(rc = __db_eval_step(&s_eval)) ){
        int i;
        int n_col;
        __db_eval_row_info(&s_eval, &n_col, 0);
        for(i=0; i<n_col; i++){
          Tcl_ListObjAppendElement(interp, ret, __db_eval_column_value(&s_eval, i));
        }
      }
      __db_eval_finalize(&s_eval);
      if( rc==TCL_BREAK ){
        Tcl_SetObjResult(interp, ret);
        rc = TCL_OK;
      }
      Tcl_DecrRefCount(ret);
    }else{
      ClientData cd2[2];
      db_eval_context_t *p;
      Tcl_Obj *var_name = 0;
      Tcl_Obj *script;

      if( objc>=5 && *(char *)Tcl_GetString(objv[3]) ){
        var_name = objv[3];
      }
      script = objv[objc-1];
      Tcl_IncrRefCount(script);

      p = (db_eval_context_t *)Tcl_Alloc(sizeof(db_eval_context_t));
      __db_eval_init(p, db, objv[2], var_name, eval_flags);

      cd2[0] = (void *)p;
      cd2[1] = (void *)script;
      rc = __db_eval_next_cmd(cd2, interp, TCL_OK);
    }
    break;
  }

  /*
  **     $db format [OPTIONS] SQL
  **
  ** Run the SQL statement(s) given as the final argument.  Use the
  ** Query Result Formatter extension of dbsql to format the output as
  ** text and return that text.
  */
  case DB_FORMAT: {
    rc = __db_qrf(db, objc, objv);
    break;
  }

  /*
  **     $db function NAME [OPTIONS] SCRIPT
  **
  ** Create a new SQL function called NAME.  Whenever that function is
  ** called, invoke SCRIPT to evaluate the function.
  **
  ** Options:
  **         --argcount N           Function has exactly N arguments
  **         --deterministic        The function is pure
  **         --directonly           Prohibit use inside triggers and views
  **         --innocuous            Has no side effects or information leaks
  **         --returntype TYPE      Specify the return type of the function
  */
  case DB_FUNCTION: {
    int flags = DBSQL_UTF8;
    sql_func_t *func;
    Tcl_Obj *script;
    char *name;
    int n_arg = -1;
    int i;
    int type = DBSQL_NULL;
    if( objc<4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NAME ?SWITCHES? SCRIPT");
      return TCL_ERROR;
    }
    for(i=3; i<(objc-1); i++){
      const char *z = Tcl_GetString(objv[i]);
      int n = strlen30(z);
      if( n>1 && strncmp(z, "-argcount",n)==0 ){
        if( i==(objc-2) ){
          Tcl_AppendResult(interp, "option requires an argument: ", z,(char*)0);
          return TCL_ERROR;
        }
        if( Tcl_GetIntFromObj(interp, objv[i+1], &n_arg) ) return TCL_ERROR;
        if( n_arg<0 ){
          Tcl_AppendResult(interp, "number of arguments must be non-negative",
                           (char*)0);
          return TCL_ERROR;
        }
        i++;
      }else
      if( n>1 && strncmp(z, "-deterministic",n)==0 ){
        flags |= DBSQL_DETERMINISTIC;
      }else
      if( n>1 && strncmp(z, "-directonly",n)==0 ){
        flags |= DBSQL_DIRECTONLY;
      }else
      if( n>1 && strncmp(z, "-innocuous",n)==0 ){
        flags |= DBSQL_INNOCUOUS;
      }else
      if( n>1 && strncmp(z, "-returntype", n)==0 ){
        const char *types[] = {"integer", "real", "text", "blob", "any", 0};
        assert( DBSQL_INTEGER==1 && DBSQL_FLOAT==2 && DBSQL_TEXT==3 );
        assert( DBSQL_BLOB==4 && DBSQL_NULL==5 );
        if( i==(objc-2) ){
          Tcl_AppendResult(interp, "option requires an argument: ", z,(char*)0);
          return TCL_ERROR;
        }
        i++;
        if( Tcl_GetIndexFromObj(interp, objv[i], types, "type", 0, &type) ){
          return TCL_ERROR;
        }
        type++;
      }else{
        Tcl_AppendResult(interp, "bad option \"", z,
            "\": must be -argcount, -deterministic, -directonly,"
            " -innocuous, or -returntype", (char*)0
        );
        return TCL_ERROR;
      }
    }

    script = objv[objc-1];
    name = Tcl_GetStringFromObj(objv[2], 0);
    func = __find_sql_func(db, name);
    if( func==0 ) return TCL_ERROR;
    if( func->script ){
      Tcl_DecrRefCount(func->script);
    }
    func->script = script;
    Tcl_IncrRefCount(script);
    func->use_eval_objv = __safe_to_use_eval_objv(script);
    func->type = type;
    rc = dbsql_create_function(db->db, name, n_arg, flags,
        func, __tcl_sql_func, 0, 0);
    if( rc!=DBSQL_OK ){
      rc = TCL_ERROR;
      Tcl_SetResult(interp, (char *)dbsql_errmsg(db->db), TCL_VOLATILE);
    }
    break;
  }

  /*
  **     $db incrblob ?-readonly? ?DB? TABLE COLUMN ROWID
  */
  case DB_INCRBLOB: {
#ifdef DBSQL_OMIT_INCRBLOB
    Tcl_AppendResult(interp, "incrblob not available in this build", (char*)0);
    return TCL_ERROR;
#else
    int is_readonly = 0;
    const char *z_db = "main";
    const char *table;
    const char *column;
    Tcl_WideInt i_row;

    /* Check for the -readonly option */
    if( objc>3 && strcmp(Tcl_GetString(objv[2]), "-readonly")==0 ){
      is_readonly = 1;
    }

    if( objc!=(5+is_readonly) && objc!=(6+is_readonly) ){
      Tcl_WrongNumArgs(interp, 2, objv, "?-readonly? ?DB? TABLE COLUMN ROWID");
      return TCL_ERROR;
    }

    if( objc==(6+is_readonly) ){
      z_db = Tcl_GetString(objv[2+is_readonly]);
    }
    table = Tcl_GetString(objv[objc-3]);
    column = Tcl_GetString(objv[objc-2]);
    rc = Tcl_GetWideIntFromObj(interp, objv[objc-1], &i_row);

    if( rc==TCL_OK ){
      rc = __create_incrblob_channel(
          interp, db, z_db, table, column, (int64_t)i_row, is_readonly
      );
    }
#endif
    break;
  }

  /*
  **     $db interrupt
  **
  ** Interrupt the execution of the inner-most SQL interpreter.  This
  ** causes the SQL statement to return an error of DBSQL_INTERRUPT.
  */
  case DB_INTERRUPT: {
    dbsql_interrupt(db->db);
    break;
  }

  /*
  **     $db nullvalue ?STRING?
  **
  ** Change text used when a NULL comes back from the database. If ?STRING?
  ** is not present, then the current string used for NULL is returned.
  ** If STRING is present, then STRING is returned.
  **
  */
  case DB_NULLVALUE: {
    if( objc!=2 && objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NULLVALUE");
      return TCL_ERROR;
    }
    if( objc==3 ){
      TCL_SIZE len;
      char *null = Tcl_GetStringFromObj(objv[2], &len);
      if( db->null ){
        Tcl_Free(db->null);
      }
      if( null && len>0 ){
        db->null = Tcl_Alloc( len + 1 );
        memcpy(db->null, null, len);
        db->null[len] = '\0';
      }else{
        db->null = 0;
      }
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(db->null, -1));
    break;
  }

  /*
  **     $db last_insert_rowid
  **
  ** Return an integer which is the ROWID for the most recent insert.
  */
  case DB_LAST_INSERT_ROWID: {
    Tcl_Obj *result;
    Tcl_WideInt rowid;
    if( objc!=2 ){
      Tcl_WrongNumArgs(interp, 2, objv, "");
      return TCL_ERROR;
    }
    rowid = dbsql_last_insert_rowid(db->db);
    result = Tcl_GetObjResult(interp);
    Tcl_SetWideIntObj(result, rowid);
    break;
  }

  /*
  ** The DB_ONECOLUMN method is implemented together with DB_EXISTS.
  */

  /*    $db progress ?N CALLBACK?
  **
  ** Invoke the given callback every N virtual machine opcodes while executing
  ** queries.
  */
  case DB_PROGRESS: {
    if( objc==2 ){
      if( db->progress ){
        Tcl_AppendResult(interp, db->progress, (char*)0);
      }
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
      dbsql_progress_handler(db->db, 0, 0, 0);
#endif
    }else if( objc==4 ){
      char *progress;
      TCL_SIZE len;
      int N;
      if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &N) ){
        return TCL_ERROR;
      };
      if( db->progress ){
        Tcl_Free(db->progress);
      }
      progress = Tcl_GetStringFromObj(objv[3], &len);
      if( progress && len>0 ){
        db->progress = Tcl_Alloc( len + 1 );
        memcpy(db->progress, progress, len+1);
      }else{
        db->progress = 0;
      }
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
      if( db->progress ){
        db->interp = interp;
        dbsql_progress_handler(db->db, N, __db_progress_handler, db);
      }else{
        dbsql_progress_handler(db->db, 0, 0, 0);
      }
#endif
    }else{
      Tcl_WrongNumArgs(interp, 2, objv, "N CALLBACK");
      return TCL_ERROR;
    }
    break;
  }

  /*    $db profile ?CALLBACK?
  **
  ** Make arrangements to invoke the CALLBACK routine after each SQL statement
  ** that has run.  The text of the SQL and the amount of elapse time are
  ** appended to CALLBACK before the script is run.
  */
  case DB_PROFILE: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->profile ){
        Tcl_AppendResult(interp, db->profile, (char*)0);
      }
    }else{
      char *profile;
      TCL_SIZE len;
      if( db->profile ){
        Tcl_Free(db->profile);
      }
      profile = Tcl_GetStringFromObj(objv[2], &len);
      if( profile && len>0 ){
        db->profile = Tcl_Alloc( len + 1 );
        memcpy(db->profile, profile, len+1);
      }else{
        db->profile = 0;
      }
#if !defined(DBSQL_OMIT_TRACE) && !defined(DBSQL_OMIT_FLOATING_POINT) && \
    !defined(DBSQL_OMIT_DEPRECATED)
      if( db->profile ){
        db->interp = interp;
        dbsql_profile(db->db, __db_profile_handler, db);
      }else{
        dbsql_profile(db->db, 0, 0);
      }
#endif
    }
    break;
  }

  /*
  **     $db rekey KEY
  **
  ** Change the encryption key on the currently open database.
  */
  case DB_REKEY: {
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "KEY");
      return TCL_ERROR;
    }
    break;
  }

  /*    $db restore ?DATABASE? FILENAME
  **
  ** Open a database file named FILENAME.  Transfer the content
  ** of FILENAME into the local database DATABASE (default: "main").
  */
  case DB_RESTORE: {
    const char *src_file;
    const char *dest_db;
    DBSQL *src;
    dbsql_backup_t *backup;
    int n_timeout = 0;

    if( objc==3 ){
      dest_db = "main";
      src_file = Tcl_GetString(objv[2]);
    }else if( objc==4 ){
      dest_db = Tcl_GetString(objv[2]);
      src_file = Tcl_GetString(objv[3]);
    }else{
      Tcl_WrongNumArgs(interp, 2, objv, "?DATABASE? FILENAME");
      return TCL_ERROR;
    }
    rc = dbsql_open_v2(src_file, &src,
                         DBSQL_OPEN_READONLY | db->open_flags, 0);
    if( rc!=DBSQL_OK ){
      Tcl_AppendResult(interp, "cannot open source database: ",
           dbsql_errmsg(src), (char*)0);
      dbsql_close(src);
      return TCL_ERROR;
    }
    backup = dbsql_backup_init(db->db, dest_db, src, "main");
    if( backup==0 ){
      Tcl_AppendResult(interp, "restore failed: ",
           dbsql_errmsg(db->db), (char*)0);
      dbsql_close(src);
      return TCL_ERROR;
    }
    while( (rc = dbsql_backup_step(backup,100))==DBSQL_OK
              || rc==DBSQL_BUSY ){
      if( rc==DBSQL_BUSY ){
        if( n_timeout++ >= 3 ) break;
        dbsql_sleep(100);
      }
    }
    dbsql_backup_finish(backup);
    if( rc==DBSQL_DONE ){
      rc = TCL_OK;
    }else if( rc==DBSQL_BUSY || rc==DBSQL_LOCKED ){
      Tcl_AppendResult(interp, "restore failed: source database busy",
                       (char*)0);
      rc = TCL_ERROR;
    }else{
      Tcl_AppendResult(interp, "restore failed: ",
           dbsql_errmsg(db->db), (char*)0);
      rc = TCL_ERROR;
    }
    dbsql_close(src);
    break;
  }

  /*
  **     $db serialize ?DATABASE?
  **
  ** Return a serialization of a database.  
  */
  case DB_SERIALIZE: {
#ifdef DBSQL_OMIT_DESERIALIZE
    Tcl_AppendResult(interp, "MEMDB not available in this build",
                     (char*)0);
    rc = TCL_ERROR;
#else
    const char *schema = objc>=3 ? Tcl_GetString(objv[2]) : "main";
    int64_t sz = 0;
    unsigned char *data;
    if( objc!=2 && objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?DATABASE?");
      rc = TCL_ERROR;
    }else{
      int need_free;
      data = dbsql_serialize(db->db, schema, &sz, DBSQL_SERIALIZE_NOCOPY);
      if( data ){
        need_free = 0;
      }else{
        data = dbsql_serialize(db->db, schema, &sz, 0);
        need_free = 1;
      }
      Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(data,sz));
      if( need_free ) dbsql_free(data);
    }
#endif
    break;
  }

  /*
  **     $db status (step|sort|autoindex|vmstep)
  **
  ** Display DBSQL_STMTSTATUS_FULLSCAN_STEP or
  ** DBSQL_STMTSTATUS_SORT for the most recent eval.
  */
  case DB_STATUS: {
    int v;
    const char *z_op;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "(step|sort|autoindex)");
      return TCL_ERROR;
    }
    z_op = Tcl_GetString(objv[2]);
    if( strcmp(z_op, "step")==0 ){
      v = db->n_step;
    }else if( strcmp(z_op, "sort")==0 ){
      v = db->n_sort;
    }else if( strcmp(z_op, "autoindex")==0 ){
      v = db->n_index;
    }else if( strcmp(z_op, "vmstep")==0 ){
      v = db->n_vm_step;
    }else{
      Tcl_AppendResult(interp,
            "bad argument: should be autoindex, step, sort or vmstep",
            (char*)0);
      return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(v));
    break;
  }

  /*
  **     $db timeout MILLESECONDS
  **
  ** Delay for the number of milliseconds specified when a file is locked.
  */
  case DB_TIMEOUT: {
    int ms;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "MILLISECONDS");
      return TCL_ERROR;
    }
    if( Tcl_GetIntFromObj(interp, objv[2], &ms) ) return TCL_ERROR;
    dbsql_busy_timeout(db->db, ms);
    break;
  }

  /*
  **     $db total_changes
  **
  ** Return the number of rows that were modified, inserted, or deleted
  ** since the database handle was created.
  */
  case DB_TOTAL_CHANGES: {
    Tcl_Obj *result;
    if( objc!=2 ){
      Tcl_WrongNumArgs(interp, 2, objv, "");
      return TCL_ERROR;
    }
    result = Tcl_GetObjResult(interp);
    Tcl_SetWideIntObj(result, dbsql_total_changes64(db->db));
    break;
  }

  /*    $db trace ?CALLBACK?
  **
  ** Make arrangements to invoke the CALLBACK routine for each SQL statement
  ** that is executed.  The text of the SQL is appended to CALLBACK before
  ** it is executed.
  */
  case DB_TRACE: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->trace ){
        Tcl_AppendResult(interp, db->trace, (char*)0);
      }
    }else{
      char *trace;
      TCL_SIZE len;
      if( db->trace ){
        Tcl_Free(db->trace);
      }
      trace = Tcl_GetStringFromObj(objv[2], &len);
      if( trace && len>0 ){
        db->trace = Tcl_Alloc( len + 1 );
        memcpy(db->trace, trace, len+1);
      }else{
        db->trace = 0;
      }
#if !defined(DBSQL_OMIT_TRACE) && !defined(DBSQL_OMIT_FLOATING_POINT) && \
    !defined(DBSQL_OMIT_DEPRECATED)
      if( db->trace ){
        db->interp = interp;
        dbsql_trace(db->db, __db_trace_handler, db);
      }else{
        dbsql_trace(db->db, 0, 0);
      }
#endif
    }
    break;
  }

  /*    $db trace_v2 ?CALLBACK? ?MASK?
  **
  ** Make arrangements to invoke the CALLBACK routine for each trace event
  ** matching the mask that is generated.  The parameters are appended to
  ** CALLBACK before it is executed.
  */
  case DB_TRACE_V2: {
    if( objc>4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK? ?MASK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( db->trace_v2 ){
        Tcl_AppendResult(interp, db->trace_v2, (char*)0);
      }
    }else{
      char *trace_v2;
      TCL_SIZE len;
      Tcl_WideInt w_mask = 0;
      if( objc==4 ){
        static const char *TTYPE_strs[] = {
          "statement", "profile", "row", "close", 0
        };
        enum TTYPE_enum {
          TTYPE_STMT, TTYPE_PROFILE, TTYPE_ROW, TTYPE_CLOSE
        };
        TCL_SIZE i;
        if( TCL_OK!=Tcl_ListObjLength(interp, objv[3], &len) ){
          return TCL_ERROR;
        }
        for(i=0; i<len; i++){
          Tcl_Obj *obj;
          int ttype;
          if( TCL_OK!=Tcl_ListObjIndex(interp, objv[3], i, &obj) ){
            return TCL_ERROR;
          }
          if( Tcl_GetIndexFromObj(interp, obj, TTYPE_strs, "trace type",
                                  0, &ttype)!=TCL_OK ){
            Tcl_WideInt w_type;
            Tcl_Obj *error = Tcl_DuplicateObj(Tcl_GetObjResult(interp));
            Tcl_IncrRefCount(error);
            if( TCL_OK==Tcl_GetWideIntFromObj(interp, obj, &w_type) ){
              Tcl_DecrRefCount(error);
              w_mask |= w_type;
            }else{
              Tcl_SetObjResult(interp, error);
              Tcl_DecrRefCount(error);
              return TCL_ERROR;
            }
          }else{
            switch( (enum TTYPE_enum)ttype ){
              case TTYPE_STMT:    w_mask |= DBSQL_TRACE_STMT;    break;
              case TTYPE_PROFILE: w_mask |= DBSQL_TRACE_PROFILE; break;
              case TTYPE_ROW:     w_mask |= DBSQL_TRACE_ROW;     break;
              case TTYPE_CLOSE:   w_mask |= DBSQL_TRACE_CLOSE;   break;
            }
          }
        }
      }else{
        w_mask = DBSQL_TRACE_STMT; /* use the "legacy" default */
      }
      if( db->trace_v2 ){
        Tcl_Free(db->trace_v2);
      }
      trace_v2 = Tcl_GetStringFromObj(objv[2], &len);
      if( trace_v2 && len>0 ){
        db->trace_v2 = Tcl_Alloc( len + 1 );
        memcpy(db->trace_v2, trace_v2, len+1);
      }else{
        db->trace_v2 = 0;
      }
#if !defined(DBSQL_OMIT_TRACE) && !defined(DBSQL_OMIT_FLOATING_POINT)
      if( db->trace_v2 ){
        db->interp = interp;
        dbsql_trace_v2(db->db, (unsigned)w_mask, __db_trace_v2_handler, db);
      }else{
        dbsql_trace_v2(db->db, 0, 0, 0);
      }
#endif
    }
    break;
  }

  /*    $db transaction [-deferred|-immediate|-exclusive] SCRIPT
  **
  ** Start a new transaction (if we are not already in the midst of a
  ** transaction) and execute the TCL script SCRIPT.  After SCRIPT
  ** completes, either commit the transaction or roll it back if SCRIPT
  ** throws an exception.  Or if no new transaction was started, do nothing.
  ** pass the exception on up the stack.
  **
  ** This command was inspired by Dave Thomas's talk on Ruby at the
  ** 2005 O'Reilly Open Source Convention (OSCON).
  */
  case DB_TRANSACTION: {
    Tcl_Obj *script;
    const char *begin = "SAVEPOINT _tcl_transaction";
    if( objc!=3 && objc!=4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "[TYPE] SCRIPT");
      return TCL_ERROR;
    }

    if( db->n_transaction==0 && objc==4 ){
      static const char *TTYPE_strs[] = {
        "deferred",   "exclusive",  "immediate", 0
      };
      enum TTYPE_enum {
        TTYPE_DEFERRED, TTYPE_EXCLUSIVE, TTYPE_IMMEDIATE
      };
      int ttype;
      if( Tcl_GetIndexFromObj(interp, objv[2], TTYPE_strs, "transaction type",
                              0, &ttype) ){
        return TCL_ERROR;
      }
      switch( (enum TTYPE_enum)ttype ){
        case TTYPE_DEFERRED:    /* no-op */;                 break;
        case TTYPE_EXCLUSIVE:   begin = "BEGIN EXCLUSIVE";  break;
        case TTYPE_IMMEDIATE:   begin = "BEGIN IMMEDIATE";  break;
      }
    }
    script = objv[objc-1];

    /* Run the dbsql BEGIN command to open a transaction or savepoint. */
    db->disable_auth++;
    rc = dbsql_exec(db->db, begin, 0, 0, 0);
    db->disable_auth--;
    if( rc!=DBSQL_OK ){
      Tcl_AppendResult(interp, dbsql_errmsg(db->db), (char*)0);
      return TCL_ERROR;
    }
    db->n_transaction++;

    /* If using NRE, schedule a callback to invoke the script script, then
    ** a second callback to commit (or rollback) the transaction or savepoint
    ** opened above. If not using NRE, evaluate the script directly, then
    ** call function __db_trans_post_cmd() to commit (or rollback) the transaction
    ** or savepoint.  */
    __add_database_ref(db);          /* __db_trans_post_cmd() calls __del_database_ref() */
    if( DB_USE_NRE() ){
      Tcl_NRAddCallback(interp, __db_trans_post_cmd, cd, 0, 0, 0);
      (void)Tcl_NREvalObj(interp, script, 0);
    }else{
      rc = __db_trans_post_cmd(&cd, interp, Tcl_EvalObjEx(interp, script, 0));
    }
    break;
  }

  /*
  **    $db unlock_notify ?script?
  */
  case DB_UNLOCK_NOTIFY: {
#ifndef DBSQL_ENABLE_UNLOCK_NOTIFY
    Tcl_AppendResult(interp, "unlock_notify not available in this build",
                     (char*)0);
    rc = TCL_ERROR;
#else
    if( objc!=2 && objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?SCRIPT?");
      rc = TCL_ERROR;
    }else{
      void (*notify)(void **, int) = 0;
      void *notify_arg = 0;

      if( db->unlock_notify ){
        Tcl_DecrRefCount(db->unlock_notify);
        db->unlock_notify = 0;
      }

      if( objc==3 ){
        notify = __db_unlock_notify;
        notify_arg = (void *)db;
        db->unlock_notify = objv[2];
        Tcl_IncrRefCount(db->unlock_notify);
      }

      if( dbsql_unlock_notify(db->db, notify, notify_arg) ){
        Tcl_AppendResult(interp, dbsql_errmsg(db->db), (char*)0);
        rc = TCL_ERROR;
      }
    }
#endif
    break;
  }

  /*
  **    $db preupdate_hook count
  **    $db preupdate_hook hook ?SCRIPT?
  **    $db preupdate_hook new INDEX
  **    $db preupdate_hook old INDEX
  */
  case DB_PREUPDATE: {
#ifndef DBSQL_ENABLE_PREUPDATE_HOOK
    Tcl_AppendResult(interp, "preupdate_hook was omitted at compile-time", 
                     (char*)0);
    rc = TCL_ERROR;
#else
    static const char *subs[] = {"count", "depth", "hook", "new", "old", 0};
    enum __db_preupdate_sub_cmd {
      PRE_COUNT, PRE_DEPTH, PRE_HOOK, PRE_NEW, PRE_OLD
    };
    int i_sub;

    if( objc<3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SUB-COMMAND ?ARGS?");
    }
    if( Tcl_GetIndexFromObj(interp, objv[2], subs, "sub-command", 0, &i_sub) ){
      return TCL_ERROR;
    }

    switch( (enum __db_preupdate_sub_cmd)i_sub ){
      case PRE_COUNT: {
        int n_col = dbsql_preupdate_count(db->db);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(n_col));
        break;
      }

      case PRE_HOOK: {
        if( objc>4 ){
          Tcl_WrongNumArgs(interp, 2, objv, "hook ?SCRIPT?");
          return TCL_ERROR;
        }
        __db_hook_cmd(interp, db, (objc==4 ? objv[3] : 0), &db->pre_update_hook);
        break;
      }

      case PRE_DEPTH: {
        Tcl_Obj *ret;
        if( objc!=3 ){
          Tcl_WrongNumArgs(interp, 3, objv, "");
          return TCL_ERROR;
        }
        ret = Tcl_NewIntObj(dbsql_preupdate_depth(db->db));
        Tcl_SetObjResult(interp, ret);
        break;
      }

      case PRE_NEW:
      case PRE_OLD: {
        int i_idx;
        dbsql_value_t *value;
        if( objc!=4 ){
          Tcl_WrongNumArgs(interp, 3, objv, "INDEX");
          return TCL_ERROR;
        }
        if( Tcl_GetIntFromObj(interp, objv[3], &i_idx) ){
          return TCL_ERROR;
        }

        if( i_sub==PRE_OLD ){
          rc = dbsql_preupdate_old(db->db, i_idx, &value);
        }else{
          assert( i_sub==PRE_NEW );
          rc = dbsql_preupdate_new(db->db, i_idx, &value);
        }

        if( rc==DBSQL_OK ){
          Tcl_Obj *obj;
          obj = Tcl_NewStringObj((char*)dbsql_value_text(value), -1);
          Tcl_SetObjResult(interp, obj);
        }else{
          Tcl_AppendResult(interp, dbsql_errmsg(db->db), (char*)0);
          return TCL_ERROR;
        }
      }
    }
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */
    break;
  }

  /*
  **    $db wal_hook ?script?
  **    $db update_hook ?script?
  **    $db rollback_hook ?script?
  */
  case DB_WAL_HOOK:
  case DB_UPDATE_HOOK:
  case DB_ROLLBACK_HOOK: {
    /* set hook to point at update_hook or rollback_hook, depending on
    ** whether [$db update_hook] or [$db rollback_hook] was invoked.
    */
    Tcl_Obj **hook = 0;
    if( choice==DB_WAL_HOOK ) hook = &db->wal_hook;
    if( choice==DB_UPDATE_HOOK ) hook = &db->update_hook;
    if( choice==DB_ROLLBACK_HOOK ) hook = &db->rollback_hook;
    if( objc>3 ){
       Tcl_WrongNumArgs(interp, 2, objv, "?SCRIPT?");
       return TCL_ERROR;
    }

    __db_hook_cmd(interp, db, (objc==3 ? objv[2] : 0), hook);
    break;
  }

  /*    $db version
  **
  ** Return the version string for this database.
  */
  case DB_VERSION: {
    int i;
    for(i=2; i<objc; i++){
      const char *arg = Tcl_GetString(objv[i]);
      /* Optional arguments to $db version are used for testing purpose */
#ifdef DBSQL_TEST
      /* $db version -use-legacy-prepare BOOLEAN
      **
      ** Turn the use of legacy dbsql_prepare() on or off.
      */
      if( strcmp(arg, "-use-legacy-prepare")==0 && i+1<objc ){
        i++;
        if( Tcl_GetBooleanFromObj(interp, objv[i], &db->legacy_prepare) ){
          return TCL_ERROR;
        }
      }else

      /* $db version -last-stmt-ptr
      **
      ** Return a string which is a hex encoding of the pointer to the
      ** most recent dbsql_stmt_t in the statement cache.
      */
      if( strcmp(arg, "-last-stmt-ptr")==0 ){
        char buf[100];
        dbsql_snprintf(sizeof(buf), buf, "%p",
                         db->stmt_list ? db->stmt_list->stmt: 0);
        Tcl_SetResult(interp, buf, TCL_VOLATILE);
      }else
#endif /* DBSQL_TEST */
      {
        Tcl_AppendResult(interp, "unknown argument: ", arg, (char*)0);
        return TCL_ERROR;
      }
    }
    if( i==2 ){   
      Tcl_SetResult(interp, (char *)dbsql_libversion(), TCL_STATIC);
    }
    break;
  }


  } /* End of the SWITCH statement */
  return rc;
}

#if DBSQL_TCL_NRE
/*
** Adaptor that provides an obj_cmd interface to the NRE-enabled
** interface implementation.
*/
static int DBSQL_TCLAPI __db_obj_cmd_adaptor(
  void *cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const*objv
){
  return Tcl_NRCallObjProc(interp, __db_obj_cmd, cd, objc, objv);
}
#endif /* DBSQL_TCL_NRE */

/*
** Issue the usage message when the "DBSQL" command arguments are
** incorrect.
*/
static int dbsql_cmd_usage(
  Tcl_Interp *interp,
  Tcl_Obj *const*objv
){
  Tcl_WrongNumArgs(interp, 1, objv,
    "HANDLE ?FILENAME? ?-vfs VFSNAME? ?-readonly BOOLEAN? ?-create BOOLEAN?"
    " ?-nofollow BOOLEAN?"
    " ?-nomutex BOOLEAN? ?-fullmutex BOOLEAN? ?-uri BOOLEAN?"
  );
  return TCL_ERROR;
}

/*
**   DBSQL DBNAME FILENAME ?-vfs VFSNAME? ?-key KEY? ?-readonly BOOLEAN?
**                           ?-create BOOLEAN? ?-nomutex BOOLEAN?
**                           ?-nofollow BOOLEAN?
**
** This is the main Tcl command.  When the "sqlite" Tcl command is
** invoked, this routine runs to process that command.
**
** The first argument, DBNAME, is an arbitrary name for a new
** database connection.  This command creates a new command named
** DBNAME that is used to control that connection.  The database
** connection is deleted when the DBNAME command is deleted.
**
** The second argument is the name of the database file.
**
*/
static int DBSQL_TCLAPI __db_main(
  void *cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const*objv
){
  dbsql_db *p;
  const char *arg;
  char *errmsg;
  int i;
  const char *file = 0;
  const char *vfs = 0;
  int flags;
  int translate_file_name = 1;
  Tcl_DString translated_filename;
  int rc;

  /* In normal use, each TCL interpreter runs in a single thread.  So
  ** by default, we can turn off mutexing on dbsql database connections.
  ** However, for testing purposes it is useful to have mutexes turned
  ** on.  So, by default, mutexes default off.  But if compiled with
  ** DBSQL_TCL_DEFAULT_FULLMUTEX then mutexes default on.
  */
#ifdef DBSQL_TCL_DEFAULT_FULLMUTEX
  flags = DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE | DBSQL_OPEN_FULLMUTEX;
#else
  flags = DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE | DBSQL_OPEN_NOMUTEX;
#endif

  if( objc==1 ) return dbsql_cmd_usage(interp, objv);
  if( objc==2 ){
    arg = Tcl_GetStringFromObj(objv[1], 0);
    if( strcmp(arg,"-version")==0 ){
      Tcl_AppendResult(interp,dbsql_libversion(), (char*)0);
      return TCL_OK;
    }
    if( strcmp(arg,"-sourceid")==0 ){
      Tcl_AppendResult(interp,dbsql_sourceid(), (char*)0);
      return TCL_OK;
    }
    if( strcmp(arg,"-has-codec")==0 ){
      Tcl_AppendResult(interp,"0",(char*)0);
      return TCL_OK;
    }
    if( arg[0]=='-' ) return dbsql_cmd_usage(interp, objv);
  }
  for(i=2; i<objc; i++){
    arg = Tcl_GetString(objv[i]);
    if( arg[0]!='-' ){
      if( file!=0 ) return dbsql_cmd_usage(interp, objv);
      file = arg;
      continue;
    }
    if( i==objc-1 ) return dbsql_cmd_usage(interp, objv);
    i++;
    if( strcmp(arg,"-key")==0 ){
      /* no-op */
    }else if( strcmp(arg, "-vfs")==0 ){
      vfs = Tcl_GetString(objv[i]);
    }else if( strcmp(arg, "-readonly")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b ){
        flags &= ~(DBSQL_OPEN_READWRITE|DBSQL_OPEN_CREATE);
        flags |= DBSQL_OPEN_READONLY;
      }else{
        flags &= ~DBSQL_OPEN_READONLY;
        flags |= DBSQL_OPEN_READWRITE;
      }
    }else if( strcmp(arg, "-create")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b && (flags & DBSQL_OPEN_READONLY)==0 ){
        flags |= DBSQL_OPEN_CREATE;
      }else{
        flags &= ~DBSQL_OPEN_CREATE;
      }
    }else if( strcmp(arg, "-nofollow")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b ){
        flags |= DBSQL_OPEN_NOFOLLOW;
      }else{
        flags &= ~DBSQL_OPEN_NOFOLLOW;
      }
    }else if( strcmp(arg, "-nomutex")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b ){
        flags |= DBSQL_OPEN_NOMUTEX;
        flags &= ~DBSQL_OPEN_FULLMUTEX;
      }else{
        flags &= ~DBSQL_OPEN_NOMUTEX;
      }
    }else if( strcmp(arg, "-fullmutex")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b ){
        flags |= DBSQL_OPEN_FULLMUTEX;
        flags &= ~DBSQL_OPEN_NOMUTEX;
      }else{
        flags &= ~DBSQL_OPEN_FULLMUTEX;
      }
    }else if( strcmp(arg, "-uri")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i], &b) ) return TCL_ERROR;
      if( b ){
        flags |= DBSQL_OPEN_URI;
      }else{
        flags &= ~DBSQL_OPEN_URI;
      }
    }else if( strcmp(arg, "-translatefilename")==0 ){
      if( Tcl_GetBooleanFromObj(interp, objv[i], &translate_file_name) ){
        return TCL_ERROR;
      }
    }else{
      Tcl_AppendResult(interp, "unknown option: ", arg, (char*)0);
      return TCL_ERROR;
    }
  }
  errmsg = 0;
  p = (dbsql_db*)Tcl_Alloc( sizeof(*p) );
  memset(p, 0, sizeof(*p));
  if( file==0 ) file = "";
  if( translate_file_name ){
    file = Tcl_TranslateFileName(interp, file, &translated_filename);
  }
  rc = dbsql_open_v2(file, &p->db, flags, vfs);
  if( translate_file_name ){
    Tcl_DStringFree(&translated_filename);
  }
  if( p->db ){
    if( DBSQL_OK!=dbsql_errcode(p->db) ){
      errmsg = dbsql_mprintf("%s", dbsql_errmsg(p->db));
      dbsql_close(p->db);
      p->db = 0;
    }
  }else{
    errmsg = dbsql_mprintf("%s", dbsql_errstr(rc));
  }
  if( p->db==0 ){
    Tcl_SetResult(interp, errmsg, TCL_VOLATILE);
    Tcl_Free((char*)p);
    dbsql_free(errmsg);
    return TCL_ERROR;
  }
  p->max_stmt = NUM_PREPARED_STMTS;
  p->open_flags = flags & DBSQL_OPEN_URI;
  p->interp = interp;
  arg = Tcl_GetStringFromObj(objv[1], 0);
  if( DB_USE_NRE() ){
    Tcl_NRCreateCommand(interp, arg, __db_obj_cmd_adaptor, __db_obj_cmd,
                        (char*)p, __db_delete_cmd);
  }else{
    Tcl_CreateObjCommand(interp, arg, __db_obj_cmd, (char*)p, __db_delete_cmd);
  }
  p->n_ref = 1;
  return TCL_OK;
}

/*
** Provide a dummy Tcl_InitStubs if we are using this as a static
** library.
*/
#ifndef USE_TCL_STUBS
# undef  Tcl_InitStubs
# define Tcl_InitStubs(a,b,c) TCL_VERSION
#endif

/*
** Make sure we have a PACKAGE_VERSION macro defined.  This will be
** defined automatically by the TEA makefile.  But other makefiles
** do not define it.
*/
#ifndef PACKAGE_VERSION
# define PACKAGE_VERSION DBSQL_VERSION
#endif

/*
** Initialize this module.
**
** This Tcl module contains only a single new Tcl command named "sqlite".
** (Hence there is no namespace.  There is no point in using a namespace
** if the extension only supplies one new name!)  The "sqlite" command is
** used to open a new dbsql database.  See the __db_main() routine above
** for additional information.
**
** The EXTERN macros are required by TCL in order to work on windows.
*/
EXTERN int Dbsql_Init(Tcl_Interp *interp){
  int rc = Tcl_InitStubs(interp, "8.5-", 0) ? TCL_OK : TCL_ERROR;
  if( rc==TCL_OK ){
    Tcl_CreateObjCommand(interp, "dbsql", (Tcl_ObjCmdProc*)__db_main, 0, 0);
#ifndef DBSQL_3_SUFFIX_ONLY
    /* The "sqlite" alias is undocumented.  It is here only to support
    ** legacy scripts.  All new scripts should use only the "DBSQL"
    ** command. */
    Tcl_CreateObjCommand(interp, "dbsql_legacy", (Tcl_ObjCmdProc*)__db_main, 0, 0);
#endif
    rc = Tcl_PkgProvide(interp, "dbsql", PACKAGE_VERSION);
  }
  return rc;
}
EXTERN int Tcldbsql_Init(Tcl_Interp *interp){ return Dbsql_Init(interp); }
EXTERN int Dbsql_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Tcldbsql_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }

/* Because it accesses the file-system and uses persistent state, dbsql
** is not considered appropriate for safe interpreters.  Hence, we cause
** the safe_init() interfaces return TCL_ERROR.
*/
EXTERN int Dbsql_SafeInit(Tcl_Interp *interp){ return TCL_ERROR; }
EXTERN int Dbsql_SafeUnload(Tcl_Interp *interp, int flags){return TCL_ERROR;}

/*
** Versions of all of the above entry points that omit the "3" at the end
** of the name.  Years ago (circa 2004) the "3" was necessary to distinguish
** dbsql version 3 from Sqlite version 2.  But two decades have elapsed.
** SQLite2 is not longer a conflict.  So it is ok to omit the "3".
**
** Omitting the "3" helps TCL find the entry point.
*/
EXTERN int Dbsql_legacy_Init(Tcl_Interp *interp){ return Dbsql_Init(interp);}
EXTERN int Tcldbsql_legacy_Init(Tcl_Interp *interp){ return Dbsql_Init(interp); }
EXTERN int Dbsql_legacy_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Tcldbsql_legacy_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Dbsql_legacy_SafeInit(Tcl_Interp *interp){ return TCL_ERROR; }
EXTERN int Dbsql_legacy_SafeUnload(Tcl_Interp *interp, int flags){return TCL_ERROR;}

/* Also variants with a lowercase "s".  I'm told that these are
** deprecated in Tcl9, but they continue to be included for backwards
** compatibility. */
EXTERN int dbsql_Init(Tcl_Interp *interp){ return Dbsql_Init(interp);}
EXTERN int dbsql_legacy_Init(Tcl_Interp *interp){ return Dbsql_Init(interp);}


/*
** If the TCLSH macro is defined, add code to make a stand-alone program.
*/
#if defined(TCLSH)

/* This is the main routine for an ordinary TCL shell.  If there are
** arguments, run the first argument as a script.  Otherwise, read TCL
** commands from standard input
*/
static const char *tclsh_main_loop(void){
  static const char mainloop[] =
    "if {[llength $argv]>=1} {\n"
#ifdef WIN32
      "set new [list]\n"
      "foreach arg $argv {\n"
        "if {[string match -* $arg] || [file exists $arg]} {\n"
          "lappend new $arg\n"
        "} else {\n"
          "set once 0\n"
          "foreach match [lsort [glob -nocomplain $arg]] {\n"
            "lappend new $match\n"
            "set once 1\n"
          "}\n"
          "if {!$once} {lappend new $arg}\n"
        "}\n"
      "}\n"
      "set argv $new\n"
      "unset new\n"
#endif
      "set argv0 [lindex $argv 0]\n"
      "set argv [lrange $argv 1 end]\n"
      "source $argv0\n"
    "} else {\n"
      "set line {}\n"
      "while {![eof stdin]} {\n"
        "if {$line!=\"\"} {\n"
          "puts -nonewline \"> \"\n"
        "} else {\n"
          "puts -nonewline \"% \"\n"
        "}\n"
        "flush stdout\n"
        "append line [gets stdin]\n"
        "if {[info complete $line]} {\n"
          "if {[catch {uplevel #0 $line} result]} {\n"
            "puts stderr \"Error: $result\"\n"
          "} elseif {$result!=\"\"} {\n"
            "puts $result\n"
          "}\n"
          "set line {}\n"
        "} else {\n"
          "append line \\n\n"
        "}\n"
      "}\n"
    "}\n"
  ;
  return mainloop;
}

#ifndef TCLSH_MAIN
# define TCLSH_MAIN main
#endif
int DBSQL_CDECL TCLSH_MAIN(int argc, char **argv){
  Tcl_Interp *interp;
  int i;
  const char *script = 0;
  char z_argc[32];
#if defined(TCLSH_INIT_PROC)
  extern const char *TCLSH_INIT_PROC(Tcl_Interp*);
#endif

#if !defined(_WIN32_WCE)
  if( getenv("DBSQL_DEBUG_BREAK") ){
    if( isatty(0) && isatty(2) ){
      fprintf(stderr,
          "attach debugger to process %d and press any key to continue.\n",
          GETPID());
      fgetc(stdin);
    }else{
#if defined(_WIN32) || defined(WIN32)
      __debug_break();
#elif defined(SIGTRAP)
      raise(SIGTRAP);
#endif
    }
  }
#endif

  /* Call dbsql_shutdown() once before doing anything else. This is to
  ** test that dbsql_shutdown() can be safely called by a process before
  ** dbsql_initialize() is. */
  dbsql_shutdown();

  Tcl_FindExecutable(argv[0]);
  Tcl_SetSystemEncoding(NULL, "utf-8");
  interp = Tcl_CreateInterp();
  Dbsql_Init(interp);

  dbsql_snprintf(sizeof(z_argc), z_argc, "%d", argc-1);
  Tcl_SetVar(interp,"argc", z_argc, TCL_GLOBAL_ONLY);
  Tcl_SetVar(interp,"argv0",argv[0],TCL_GLOBAL_ONLY);
  Tcl_SetVar(interp,"argv", "", TCL_GLOBAL_ONLY);
  for(i=1; i<argc; i++){
    Tcl_SetVar(interp, "argv", argv[i],
        TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT | TCL_APPEND_VALUE);
  }
#if defined(TCLSH_INIT_PROC)
  script = TCLSH_INIT_PROC(interp);
#endif
  if( script==0 ){
    script = tclsh_main_loop();
  }
  if( Tcl_GlobalEval(interp, script)!=TCL_OK ){
    const char *info = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if( info==0 ) info = Tcl_GetStringResult(interp);
    fprintf(stderr,"%s: %s\n", *argv, info);
    return 1;
  }
  return 0;
}
#endif /* TCLSH */
