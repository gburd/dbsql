/*
** 2007 August 15
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
** This file contains code used to implement test interfaces to the
** memory allocation subsystem.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
** This structure is used to encapsulate the global state variables used 
** by malloc() fault simulation.
*/
static struct mem_fault_t {
  int i_countdown;         /* Number of pending successes before a failure */
  int n_repeat;            /* Number of times to repeat the failure */
  int n_benign;            /* Number of benign failures seen since last config */
  int n_fail;              /* Number of failures seen since last config */
  int n_ok_before;          /* Successful allocations prior to the first fault */
  int n_ok_after;           /* Successful allocations after a fault */
  u_int8_t enable;              /* True if enabled */
  int is_installed;        /* True if the fault simulation layer is installed */
  int is_benign_mode;       /* True if malloc failures are considered benign */
  dbsql_mem_methods_t m;  /* 'Real' malloc implementation */
} memfault;

/*
** This routine exists as a place to set a breakpoint that will
** fire on any simulated malloc() failure.
*/
static void __fault(void){
  static u_int64_t cnt = 0;
  cnt++;
  if( cnt>((u_int64_t)1<<63) ) abort();
}

/*
** This routine exists as a place to set a breakpoint that will
** fire the first time any malloc() fails on a single test case.
** The __fault() routine above runs on every malloc() failure.
** This routine only runs on the first such failure.
*/
static void __first_fault(void){
  static u_int64_t cnt2 = 0;
  cnt2++;
  if( cnt2>((u_int64_t)1<<63) ) abort();
}

/*
** Check to see if a fault should be simulated.  Return true to simulate
** the fault.  Return false if the fault should not be simulated.
*/
static int __faultsim_step(void){
  if( likely(!memfault.enable) ){
    memfault.n_ok_after++;
    return 0;
  }
  if( memfault.i_countdown>0 ){
    memfault.i_countdown--;
    memfault.n_ok_before++;
    return 0;
  }
  if( memfault.n_fail==0 ) __first_fault();
  __fault();
  memfault.n_fail++;
  if( memfault.is_benign_mode>0 ){
    memfault.n_benign++;
  }
  memfault.n_repeat--;
  if( memfault.n_repeat<=0 ){
    memfault.enable = 0;
  }
  return 1;  
}

/*
** A version of dbsql_mem_methods_t.x_malloc() that includes fault simulation
** logic.
*/
static void *__faultsim_malloc(int n){
  void *p = 0;
  if( !__faultsim_step() ){
    p = memfault.m.x_malloc(n);
  }
  return p;
}


/*
** A version of dbsql_mem_methods_t.x_realloc() that includes fault simulation
** logic.
*/
static void *__faultsim_realloc(void *old, int n){
  void *p = 0;
  if( !__faultsim_step() ){
    p = memfault.m.x_realloc(old, n);
  }
  return p;
}

/*
** This routine configures the malloc failure simulation.  After
** calling this routine, the next n_delay mallocs will succeed, followed
** by a block of n_repeat failures, after which malloc() calls will begin
** to succeed again.
*/
static void __faultsim_config(int n_delay, int n_repeat){
  memfault.i_countdown = n_delay;
  memfault.n_repeat = n_repeat;
  memfault.n_benign = 0;
  memfault.n_fail = 0;
  memfault.n_ok_before = 0;
  memfault.n_ok_after = 0;
  memfault.enable = n_delay>=0;

  /* Sometimes, when running multi-threaded tests, the is_benign_mode 
  ** variable is not properly incremented/decremented so that it is
  ** 0 when not inside a benign malloc block. This doesn't affect
  ** the multi-threaded tests, as they do not use this system. But
  ** it does affect OOM tests run later in the same process. So
  ** zero the variable here, just to be sure.
  */
  memfault.is_benign_mode = 0;
}

/*
** Return the number of faults (both hard and benign faults) that have
** occurred since the injector was last configured.
*/
static int __faultsim_failures(void){
  return memfault.n_fail;
}

/*
** Return the number of benign faults that have occurred since the
** injector was last configured.
*/
static int __faultsim_benign_failures(void){
  return memfault.n_benign;
}

/*
** Return the number of successes that will occur before the next failure.
** If no failures are scheduled, return -1.
*/
static int __faultsim_pending(void){
  if( memfault.enable ){
    return memfault.i_countdown;
  }else{
    return -1;
  }
}


static void __faultsim_begin_benign(void){
  memfault.is_benign_mode++;
}
static void __faultsim_end_benign(void){
  memfault.is_benign_mode--;
}

/*
** Add or remove the fault-simulation layer using dbsql_config(). If
** the argument is non-zero, the 
*/
static int __faultsim_install(int install){
  int rc;

  install = (install ? 1 : 0);
  assert(memfault.is_installed==1 || memfault.is_installed==0);

  if( install==memfault.is_installed ){
    return DBSQL_ERROR;
  }

  if( install ){
    rc = dbsql_config(DBSQL_CONFIG_GETMALLOC, &memfault.m);
    assert(memfault.m.x_malloc);
    if( rc==DBSQL_OK ){
      dbsql_mem_methods_t m = memfault.m;
      m.x_malloc = __faultsim_malloc;
      m.x_realloc = __faultsim_realloc;
      rc = dbsql_config(DBSQL_CONFIG_MALLOC, &m);
    }
    dbsql_test_control(DBSQL_TESTCTRL_BENIGN_MALLOC_HOOKS, 
        __faultsim_begin_benign, __faultsim_end_benign
    );
  }else{
    dbsql_mem_methods_t m2;
    assert(memfault.m.x_malloc);

    /* One should be able to reset the default memory allocator by storing
    ** a zeroed allocator then calling GETMALLOC. */
    memset(&m2, 0, sizeof(m2));
    dbsql_config(DBSQL_CONFIG_MALLOC, &m2);
    dbsql_config(DBSQL_CONFIG_GETMALLOC, &m2);
    assert( memcmp(&m2, &memfault.m, sizeof(m2))==0 );

    rc = dbsql_config(DBSQL_CONFIG_MALLOC, &memfault.m);
    dbsql_test_control(DBSQL_TESTCTRL_BENIGN_MALLOC_HOOKS,
        (void*)0, (void*)0);
  }

  if( rc==DBSQL_OK ){
    memfault.is_installed = 1;
  }
  return rc;
}

#ifdef DBSQL_TEST

/*
** This function is implemented in main.c. Returns a pointer to a static
** buffer containing the symbolic dbsql error code that corresponds to
** the least-significant 8-bits of the integer passed as an argument.
** For example:
**
**   __err_name(1) -> "DBSQL_ERROR"
*/
extern const char *__err_name(int);

/*
** Transform pointers to text and back again
*/
static void __pointer_to_text(void *p, char *z){
  static const char hex[] = "0123456789abcdef";
  int i, k;
  unsigned int u;
  u_int64_t n;
  if( p==0 ){
    strcpy(z, "0");
    return;
  }
  if( sizeof(n)==sizeof(p) ){
    memcpy(&n, &p, sizeof(p));
  }else if( sizeof(u)==sizeof(p) ){
    memcpy(&u, &p, sizeof(u));
    n = u;
  }else{
    assert( 0 );
  }
  for(i=0, k=sizeof(p)*2-1; i<sizeof(p)*2; i++, k--){
    z[k] = hex[n&0xf];
    n >>= 4;
  }
  z[sizeof(p)*2] = 0;
}
static int __hex_to_int_local(int h){
  if( h>='0' && h<='9' ){
    return h - '0';
  }else if( h>='a' && h<='f' ){
    return h - 'a' + 10;
  }else{
    return -1;
  }
}
static int __text_to_pointer(const char *z, void **pp){
  u_int64_t n = 0;
  int i;
  unsigned int u;
  for(i=0; i<sizeof(void*)*2 && z[0]; i++){
    int v;
    v = __hex_to_int_local(*z++);
    if( v<0 ) return TCL_ERROR;
    n = n*16 + v;
  }
  if( *z!=0 ) return TCL_ERROR;
  if( sizeof(n)==sizeof(*pp) ){
    memcpy(pp, &n, sizeof(n));
  }else if( sizeof(u)==sizeof(*pp) ){
    u = (unsigned int)n;
    memcpy(pp, &u, sizeof(u));
  }else{
    assert( 0 );
  }
  return TCL_OK;
}

/*
** Usage:    dbsql_malloc  NBYTES
**
** Raw test interface for dbsql_malloc().
*/
static int DBSQL_TCLAPI test_malloc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_byte;
  void *p;
  char out[100];
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTES");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &n_byte) ) return TCL_ERROR;
  p = dbsql_malloc((unsigned)n_byte);
  __pointer_to_text(p, out);
  Tcl_AppendResult(interp, out, NULL);
  return TCL_OK;
}

/*
** Usage:    dbsql_realloc  PRIOR  NBYTES
**
** Raw test interface for dbsql_realloc().
*/
static int DBSQL_TCLAPI test_realloc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_byte;
  void *prior, *p;
  char out[100];
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PRIOR NBYTES");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &n_byte) ) return TCL_ERROR;
  if( __text_to_pointer(Tcl_GetString(objv[1]), &prior) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  p = dbsql_realloc(prior, (unsigned)n_byte);
  __pointer_to_text(p, out);
  Tcl_AppendResult(interp, out, NULL);
  return TCL_OK;
}

/*
** Usage:    dbsql_free  PRIOR
**
** Raw test interface for dbsql_free().
*/
static int DBSQL_TCLAPI test_free(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *prior;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PRIOR");
    return TCL_ERROR;
  }
  if( __text_to_pointer(Tcl_GetString(objv[1]), &prior) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  dbsql_free(prior);
  return TCL_OK;
}

/*
** These routines are in test_hexio.c
*/
int __test_hex_to_bin(const char *, int, char *);
int __test_bin_to_hex(char*,int);

/*
** Usage:    memset  ADDRESS  SIZE  HEX
**
** Set a chunk of memory (obtained from malloc, probably) to a
** specified hex pattern.
*/
static int DBSQL_TCLAPI test_memset(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *p;
  int size, i;
  TCL_SIZE n;
  char *hex;
  char *out;
  char bin[100];

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "ADDRESS SIZE HEX");
    return TCL_ERROR;
  }
  if( __text_to_pointer(Tcl_GetString(objv[1]), &p) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &size) ){
    return TCL_ERROR;
  }
  if( size<=0 ){
    Tcl_AppendResult(interp, "size must be positive", (char*)0);
    return TCL_ERROR;
  }
  hex = Tcl_GetStringFromObj(objv[3], &n);
  if( n>sizeof(bin)*2 ) n = sizeof(bin)*2;
  n = __test_hex_to_bin(hex, (int)n, bin);
  if( n==0 ){
    Tcl_AppendResult(interp, "no data", (char*)0);
    return TCL_ERROR;
  }
  out = p;
  for(i=0; i<size; i++){
    out[i] = bin[i%n];
  }
  return TCL_OK;
}

/*
** Usage:    memget  ADDRESS  SIZE
**
** Return memory as hexadecimal text.
*/
static int DBSQL_TCLAPI test_memget(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *p;
  int size, n;
  char *bin;
  char hex[100];

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "ADDRESS SIZE");
    return TCL_ERROR;
  }
  if( __text_to_pointer(Tcl_GetString(objv[1]), &p) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &size) ){
    return TCL_ERROR;
  }
  if( size<=0 ){
    Tcl_AppendResult(interp, "size must be positive", (char*)0);
    return TCL_ERROR;
  }
  bin = p;
  while( size>0 ){
    if( size>(sizeof(hex)-1)/2 ){
      n = (sizeof(hex)-1)/2;
    }else{
      n = size;
    }
    memcpy(hex, bin, n);
    bin += n;
    size -= n;
    __test_bin_to_hex(hex, n);
    Tcl_AppendResult(interp, hex, (char*)0);
  }
  return TCL_OK;
}

/*
** Usage:    dbsql_memory_used
**
** Raw test interface for dbsql_memory_used().
*/
static int DBSQL_TCLAPI test_memory_used(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(dbsql_memory_used()));
  return TCL_OK;
}

/*
** Usage:    dbsql_memory_highwater ?RESETFLAG?
**
** Raw test interface for dbsql_memory_highwater().
*/
static int DBSQL_TCLAPI test_memory_highwater(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int reset_flag = 0;
  if( objc!=1 && objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?RESET?");
    return TCL_ERROR;
  }
  if( objc==2 ){
    if( Tcl_GetBooleanFromObj(interp, objv[1], &reset_flag) ) return TCL_ERROR;
  } 
  Tcl_SetObjResult(interp, 
     Tcl_NewWideIntObj(dbsql_memory_highwater(reset_flag)));
  return TCL_OK;
}

/*
** Usage:    dbsql_memdebug_backtrace DEPTH
**
** Set the depth of backtracing.  If DBSQL_MEMDEBUG is not defined
** then this routine is a no-op.
*/
static int DBSQL_TCLAPI test_memdebug_backtrace(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int depth;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DEPT");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &depth) ) return TCL_ERROR;
#ifdef DBSQL_MEMDEBUG
  {
    extern void __memdebug_backtrace(int);
    __memdebug_backtrace(depth);
  }
#endif
  return TCL_OK;
}

/*
** Usage:    dbsql_memdebug_dump  FILENAME
**
** Write a summary of unfreed memory to FILENAME.
*/
static int DBSQL_TCLAPI test_memdebug_dump(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }
#if defined(DBSQL_MEMDEBUG) || defined(DBSQL_MEMORY_SIZE) \
     || defined(DBSQL_POW2_MEMORY_SIZE)
  {
    extern void __memdebug_dump(const char*);
    __memdebug_dump(Tcl_GetString(objv[1]));
  }
#endif
  return TCL_OK;
}

/*
** Usage:    dbsql_memdebug_malloc_count
**
** Return the total number of times malloc() has been called.
*/
static int DBSQL_TCLAPI test_memdebug_malloc_count(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_malloc = -1;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
#if defined(DBSQL_MEMDEBUG)
  {
    extern int __memdebug_malloc_count();
    n_malloc = __memdebug_malloc_count();
  }
#endif
  Tcl_SetObjResult(interp, Tcl_NewIntObj(n_malloc));
  return TCL_OK;
}


/*
** Usage:    dbsql_memdebug_fail  COUNTER  ?OPTIONS?
**
** where options are:
**
**     -repeat    <count>
**     -benigncnt <varname>
**
** Arrange for a simulated malloc() failure after COUNTER successes.
** If a repeat count is specified, the fault is repeated that many
** times.
**
** Each call to this routine overrides the prior counter value.
** This routine returns the number of simulated failures that have
** happened since the previous call to this routine.
**
** To disable simulated failures, use a COUNTER of -1.
*/
static int DBSQL_TCLAPI test_memdebug_fail(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int ii;
  int i_fail;
  int n_repeat = 1;
  Tcl_Obj *benign_cnt = 0;
  int n_benign;
  int n_fail = 0;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "COUNTER ?OPTIONS?");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &i_fail) ) return TCL_ERROR;

  for(ii=2; ii<objc; ii+=2){
    TCL_SIZE n_option;
    char *option = Tcl_GetStringFromObj(objv[ii], &n_option);
    char *err = 0;

    if( n_option>1 && strncmp(option, "-repeat", n_option)==0 ){
      if( ii==(objc-1) ){
        err = "option requires an argument: ";
      }else{
        if( Tcl_GetIntFromObj(interp, objv[ii+1], &n_repeat) ){
          return TCL_ERROR;
        }
      }
    }else if( n_option>1 && strncmp(option, "-benigncnt", n_option)==0 ){
      if( ii==(objc-1) ){
        err = "option requires an argument: ";
      }else{
        benign_cnt = objv[ii+1];
      }
    }else{
      err = "unknown option: ";
    }

    if( err ){
      Tcl_AppendResult(interp, err, option, NULL);
      return TCL_ERROR;
    }
  }
  
  n_benign = __faultsim_benign_failures();
  n_fail = __faultsim_failures();
  __faultsim_config(i_fail, n_repeat);

  if( benign_cnt ){
    Tcl_ObjSetVar2(interp, benign_cnt, 0, Tcl_NewIntObj(n_benign), 0);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(n_fail));
  return TCL_OK;
}

/*
** Usage:    dbsql_memdebug_pending
**
** Return the number of malloc() calls that will succeed before a 
** simulated failure occurs. A negative return value indicates that
** no malloc() failure is scheduled.
*/
static int DBSQL_TCLAPI test_memdebug_pending(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_pending;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
  n_pending = __faultsim_pending();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(n_pending));
  return TCL_OK;
}

/*
** The following global variable keeps track of the number of tests
** that have run.  This variable is only useful when running in the
** debugger.
*/
static int dbsql_memdebug_title_count = 0;

/*
** Usage:    dbsql_memdebug_settitle TITLE
**
** Set a title string stored with each allocation.  The TITLE is
** typically the name of the test that was running when the
** allocation occurred.  The TITLE is stored with the allocation
** and can be used to figure out which tests are leaking memory.
**
** Each title overwrite the previous.
*/
static int DBSQL_TCLAPI test_memdebug_settitle(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_memdebug_title_count++;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "TITLE");
    return TCL_ERROR;
  }
#ifdef DBSQL_MEMDEBUG
  {
    const char *title;
    extern int __memdebug_settitle(const char*);
    title = Tcl_GetString(objv[1]);
    __memdebug_settitle(title);
  }
#endif
  return TCL_OK;
}

#define MALLOC_LOG_FRAMES  10 
#define MALLOC_LOG_KEYINTS (                                              \
    10 * ((sizeof(int)>=sizeof(void*)) ? 1 : sizeof(void*)/sizeof(int))   \
)
static Tcl_HashTable __a_malloc_log;
static int __malloc_log_enabled = 0;

typedef struct malloc_log_t malloc_log_t;
struct malloc_log_t {
  int n_call;
  int n_byte;
};

#ifdef DBSQL_MEMDEBUG
static void test_memdebug_callback(int n_byte, int n_frame, void **frames){
  if( __malloc_log_enabled ){
    malloc_log_t *p_log;
    Tcl_HashEntry *entry;
    int is_new;

    int keys[MALLOC_LOG_KEYINTS];
    unsigned int n_key = sizeof(int)*MALLOC_LOG_KEYINTS;

    memset(keys, 0, n_key);
    if( (sizeof(void*)*n_frame)<n_key ){
      n_key = n_frame*sizeof(void*);
    }
    memcpy(keys, frames, n_key);

    entry = Tcl_CreateHashEntry(&__a_malloc_log, (const char *)keys, &is_new);
    if( is_new ){
      p_log = (malloc_log_t *)Tcl_Alloc(sizeof(malloc_log_t));
      memset(p_log, 0, sizeof(malloc_log_t));
      Tcl_SetHashValue(entry, (ClientData)p_log);
    }else{
      p_log = (malloc_log_t *)Tcl_GetHashValue(entry);
    }

    p_log->n_call++;
    p_log->n_byte += n_byte;
  }
}
#endif /* DBSQL_MEMDEBUG */

static void test_memdebug_log_clear(void){
  Tcl_HashSearch search;
  Tcl_HashEntry *entry;
  for(
    entry=Tcl_FirstHashEntry(&__a_malloc_log, &search);
    entry;
    entry=Tcl_NextHashEntry(&search)
  ){
    malloc_log_t *p_log = (malloc_log_t *)Tcl_GetHashValue(entry);
    Tcl_Free((char *)p_log);
  }
  Tcl_DeleteHashTable(&__a_malloc_log);
  Tcl_InitHashTable(&__a_malloc_log, MALLOC_LOG_KEYINTS);
}

static int DBSQL_TCLAPI test_memdebug_log(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static int is_init = 0;
  int i_sub;

  static const char *MB_strs[] = { "start", "stop", "dump", "clear", "sync" };
  enum MB_enum { 
      MB_LOG_START, MB_LOG_STOP, MB_LOG_DUMP, MB_LOG_CLEAR, MB_LOG_SYNC 
  };

  if( !is_init ){
#ifdef DBSQL_MEMDEBUG
    extern void __memdebug_backtrace_callback(
        void (*backtrace_fn)(int, int, void **));
    __memdebug_backtrace_callback(test_memdebug_callback);
#endif
    Tcl_InitHashTable(&__a_malloc_log, MALLOC_LOG_KEYINTS);
    is_init = 1;
  }

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND ...");
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], MB_strs, "sub-command", 0, &i_sub) ){
    return TCL_ERROR;
  }

  switch( (enum MB_enum)i_sub ){
    case MB_LOG_START:
      __malloc_log_enabled = 1;
      break;
    case MB_LOG_STOP:
      __malloc_log_enabled = 0;
      break;
    case MB_LOG_DUMP: {
      Tcl_HashSearch search;
      Tcl_HashEntry *entry;
      Tcl_Obj *ret = Tcl_NewObj();

      assert(sizeof(Tcl_WideInt)>=sizeof(void*));

      for(
        entry=Tcl_FirstHashEntry(&__a_malloc_log, &search);
        entry;
        entry=Tcl_NextHashEntry(&search)
      ){
        Tcl_Obj *elems[MALLOC_LOG_FRAMES+2];
        malloc_log_t *p_log = (malloc_log_t *)Tcl_GetHashValue(entry);
        Tcl_WideInt *keys = (Tcl_WideInt *)Tcl_GetHashKey(&__a_malloc_log, entry);
        int ii;
  
        elems[0] = Tcl_NewIntObj(p_log->n_call);
        elems[1] = Tcl_NewIntObj(p_log->n_byte);
        for(ii=0; ii<MALLOC_LOG_FRAMES; ii++){
          elems[ii+2] = Tcl_NewWideIntObj(keys[ii]);
        }

        Tcl_ListObjAppendElement(interp, ret,
            Tcl_NewListObj(MALLOC_LOG_FRAMES+2, elems)
        );
      }

      Tcl_SetObjResult(interp, ret);
      break;
    }
    case MB_LOG_CLEAR: {
      test_memdebug_log_clear();
      break;
    }

    case MB_LOG_SYNC: {
#ifdef DBSQL_MEMDEBUG
      extern void __memdebug_sync();
      test_memdebug_log_clear();
      __malloc_log_enabled = 1;
      __memdebug_sync();
#endif
      break;
    }
  }

  return TCL_OK;
}

/*
** Usage:    dbsql_config_pagecache SIZE N
**
** Set the page-cache memory buffer using DBSQL_CONFIG_PAGECACHE.
** The buffer is static and is of limited size.  N might be
** adjusted downward as needed to accommodate the requested size.
** The revised value of N is returned.
**
** A negative SIZE causes the buffer pointer to be NULL.
*/
static int DBSQL_TCLAPI test_config_pagecache(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int sz, N;
  Tcl_Obj *res;
  static char *buf = 0;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SIZE N");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &N) ) return TCL_ERROR;
  free(buf);
  buf = 0;

  /* Set the return value */
  res = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, res, Tcl_NewIntObj(__global_config.sz_page));
  Tcl_ListObjAppendElement(0, res, Tcl_NewIntObj(__global_config.n_page));
  Tcl_SetObjResult(interp, res);

  if( sz<0 ){
    dbsql_config(DBSQL_CONFIG_PAGECACHE, (void*)0, 0, 0);
  }else{
    buf = malloc( sz*N );
    dbsql_config(DBSQL_CONFIG_PAGECACHE, buf, sz, N);
  }
  return TCL_OK;
}

/*
** Usage:    dbsql_config_alt_pcache INSTALL_FLAG DISCARD_CHANCE PRNG_SEED
**
** Set up the alternative test page cache.  Install if INSTALL_FLAG is
** true and uninstall (reverting to the default page cache) if INSTALL_FLAG
** is false.  DISCARD_CHANGE is an integer between 0 and 100 inclusive
** which determines the chance of discarding a page when unpinned.  100
** is certainty.  0 is never.  PRNG_SEED is the pseudo-random number generator
** seed.
*/
static int DBSQL_TCLAPI test_alt_pcache(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int install_flag;
  int discard_chance = 0;
  int __prng_seed = 0;
  int high_stress = 0;
  extern void __install_test_p_cache(int,unsigned,unsigned,unsigned);
  if( objc<2 || objc>5 ){
    Tcl_WrongNumArgs(interp, 1, objv, 
        "INSTALLFLAG DISCARDCHANCE PRNGSEEED HIGHSTRESS");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &install_flag) ) return TCL_ERROR;
  if( objc>=3 && Tcl_GetIntFromObj(interp, objv[2], &discard_chance) ){
     return TCL_ERROR;
  }
  if( objc>=4 && Tcl_GetIntFromObj(interp, objv[3], &__prng_seed) ){
     return TCL_ERROR;
  }
  if( objc>=5 && Tcl_GetIntFromObj(interp, objv[4], &high_stress) ){
    return TCL_ERROR;
  }
  if( discard_chance<0 || discard_chance>100 ){
    Tcl_AppendResult(interp, "discard-chance should be between 0 and 100",
                     (char*)0);
    return TCL_ERROR;
  }
  __install_test_p_cache(install_flag, (unsigned)discard_chance, (unsigned)__prng_seed,
                    (unsigned)high_stress);
  return TCL_OK;
}

/*
** Usage:    dbsql_config_memstatus BOOLEAN
**
** Enable or disable memory status reporting using DBSQL_CONFIG_MEMSTATUS.
*/
static int DBSQL_TCLAPI test_config_memstatus(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int enable, rc;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &enable) ) return TCL_ERROR;
  rc = dbsql_config(DBSQL_CONFIG_MEMSTATUS, enable);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** Usage:    dbsql_config_lookaside  SIZE  COUNT
**
*/
static int DBSQL_TCLAPI test_config_lookaside(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int sz, cnt;
  Tcl_Obj *ret;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SIZE COUNT");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &cnt) ) return TCL_ERROR;
  ret = Tcl_NewObj();
  Tcl_ListObjAppendElement(
      interp, ret, Tcl_NewIntObj(__global_config.sz_lookaside)
  );
  Tcl_ListObjAppendElement(
      interp, ret, Tcl_NewIntObj(__global_config.n_lookaside)
  );
  dbsql_config(DBSQL_CONFIG_LOOKASIDE, sz, cnt);
  Tcl_SetObjResult(interp, ret);
  return TCL_OK;
}


/*
** Usage:    dbsql_db_config_lookaside  CONNECTION  BUFID  SIZE  COUNT
**
** There are two static buffers with BUFID 1 and 2.   Each static buffer
** is 10KB in size.  A BUFID of 0 indicates that the buffer should be NULL
** which will cause dbsql_db_config() to allocate space on its own.
*/
static int DBSQL_TCLAPI test_db_config_lookaside(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int sz, cnt;
  DBSQL *db;
  int bufid;
  static char bufs[2][10000];
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  if( objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BUFID SIZE COUNT");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &bufid) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[3], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[4], &cnt) ) return TCL_ERROR;
  if( bufid==0 ){
    rc = dbsql_db_config(db, DBSQL_DBCONFIG_LOOKASIDE, (void*)0, sz, cnt);
  }else if( bufid>=1 && bufid<=2 && sz*cnt<=sizeof(bufs[0]) ){
    rc = dbsql_db_config(db, DBSQL_DBCONFIG_LOOKASIDE, bufs[bufid], sz,cnt);
  }else{
    Tcl_AppendResult(interp, "illegal arguments - see documentation", (char*)0);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** Usage:    dbsql_config_heap NBYTE NMINALLOC
*/
static int DBSQL_TCLAPI test_config_heap(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static char *buf; /* Use this memory */
  int n_byte;         /* Size of buffer to pass to dbsql_config() */
  int n_min_alloc;     /* Size of minimum allocation */
  int rc;            /* Return code of dbsql_config() */

  Tcl_Obj * CONST *args = &objv[1];
  int n_arg = objc-1;

  if( n_arg!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTE NMINALLOC");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, args[0], &n_byte) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, args[1], &n_min_alloc) ) return TCL_ERROR;

  if( n_byte==0 ){
    free( buf );
    buf = 0;
    rc = dbsql_config(DBSQL_CONFIG_HEAP, (void*)0, 0, 0);
  }else{
    buf = realloc(buf, n_byte);
    rc = dbsql_config(DBSQL_CONFIG_HEAP, buf, n_byte, n_min_alloc);
  }

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:    dbsql_config_heap_size NBYTE
*/
static int DBSQL_TCLAPI test_config_heap_size(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int n_byte;         /* Size to pass to dbsql_config() */
  int rc;            /* Return code of dbsql_config() */

  Tcl_Obj * CONST *args = &objv[1];
  int n_arg = objc-1;

  if( n_arg!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTE");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, args[0], &n_byte) ) return TCL_ERROR;

  rc = dbsql_config(DBSQL_CONFIG_WIN32_HEAPSIZE, n_byte);

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:    dbsql_config_error  [DB]
**
** Invoke dbsql_config() or dbsql_db_config() with invalid
** opcodes and verify that they return errors.
*/
static int DBSQL_TCLAPI test_config_error(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);

  if( objc!=2 && objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "[DB]");
    return TCL_ERROR;
  }
  if( objc==2 ){
    if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
    if( dbsql_db_config(db, 99999)!=DBSQL_ERROR ){
      Tcl_AppendResult(interp, 
            "dbsql_db_config(db, 99999) does not return DBSQL_ERROR",
            (char*)0);
      return TCL_ERROR;
    }
  }else{
    if( dbsql_config(99999)!=DBSQL_ERROR ){
      Tcl_AppendResult(interp, 
          "dbsql_config(99999) does not return DBSQL_ERROR",
          (char*)0);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

/*
** Usage:    dbsql_config_uri  BOOLEAN
**
** Enables or disables interpretation of URI parameters by default using
** DBSQL_CONFIG_URI.
*/
static int DBSQL_TCLAPI test_config_uri(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int open_uri;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &open_uri) ){
    return TCL_ERROR;
  }

  rc = dbsql_config(DBSQL_CONFIG_URI, open_uri);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);

  return TCL_OK;
}

/*
** Usage:    dbsql_config_cis  BOOLEAN
**
** Enables or disables the use of the covering-index scan optimization.
** DBSQL_CONFIG_COVERING_INDEX_SCAN.
*/
static int DBSQL_TCLAPI test_config_cis(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int use_cis;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &use_cis) ){
    return TCL_ERROR;
  }

  rc = dbsql_config(DBSQL_CONFIG_COVERING_INDEX_SCAN, use_cis);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);

  return TCL_OK;
}

/*
** Usage:    dbsql_config_pmasz  INTEGER
**
** Set the minimum PMA size.
*/
static int DBSQL_TCLAPI test_config_pmasz(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int i_pma_sz;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &i_pma_sz) ){
    return TCL_ERROR;
  }

  rc = dbsql_config(DBSQL_CONFIG_PMASZ, i_pma_sz);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);

  return TCL_OK;
}


/*
** Usage:    dbsql_dump_memsys3  FILENAME
**           dbsql_dump_memsys5  FILENAME
**
** Write a summary of unfreed memsys3 allocations to FILENAME.
*/
static int DBSQL_TCLAPI test_dump_memsys3(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }

  switch( DBSQL_PTR_TO_INT(clientData) ){
    case 3: {
#ifdef DBSQL_ENABLE_MEMSYS3
      extern void __memsys3_dump(const char*);
      __memsys3_dump(Tcl_GetString(objv[1]));
      break;
#endif
    }
    case 5: {
#ifdef DBSQL_ENABLE_MEMSYS5
      extern void __memsys5_dump(const char*);
      __memsys5_dump(Tcl_GetString(objv[1]));
      break;
#endif
    }
  }
  return TCL_OK;
}

/*
** Usage:    dbsql_status  OPCODE  RESETFLAG
**
** Return a list of three elements which are the dbsql_status() return
** code, the current value, and the high-water mark value.
*/
static int DBSQL_TCLAPI test_status(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc, i_value, mx_value;
  int i, op = 0, reset_flag;
  const char *op_name;
  static const struct {
    const char *name;
    int op;
  } ops[] = {
    { "DBSQL_STATUS_MEMORY_USED",         DBSQL_STATUS_MEMORY_USED         },
    { "DBSQL_STATUS_MALLOC_SIZE",         DBSQL_STATUS_MALLOC_SIZE         },
    { "DBSQL_STATUS_PAGECACHE_USED",      DBSQL_STATUS_PAGECACHE_USED      },
    { "DBSQL_STATUS_PAGECACHE_OVERFLOW",  DBSQL_STATUS_PAGECACHE_OVERFLOW  },
    { "DBSQL_STATUS_PAGECACHE_SIZE",      DBSQL_STATUS_PAGECACHE_SIZE      },
    { "DBSQL_STATUS_SCRATCH_USED",        DBSQL_STATUS_SCRATCH_USED        },
    { "DBSQL_STATUS_SCRATCH_OVERFLOW",    DBSQL_STATUS_SCRATCH_OVERFLOW    },
    { "DBSQL_STATUS_SCRATCH_SIZE",        DBSQL_STATUS_SCRATCH_SIZE        },
    { "DBSQL_STATUS_PARSER_STACK",        DBSQL_STATUS_PARSER_STACK        },
    { "DBSQL_STATUS_MALLOC_COUNT",        DBSQL_STATUS_MALLOC_COUNT        },
  };
  Tcl_Obj *result;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PARAMETER RESETFLAG");
    return TCL_ERROR;
  }
  op_name = Tcl_GetString(objv[1]);
  for(i=0; i<ARRAY_SIZE(ops); i++){
    if( strcmp(ops[i].name, op_name)==0 ){
      op = ops[i].op;
      break;
    }
  }
  if( i>=ARRAY_SIZE(ops) ){
    if( Tcl_GetIntFromObj(interp, objv[1], &op) ) return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[2], &reset_flag) ) return TCL_ERROR;
  i_value = 0;
  mx_value = 0;
  rc = dbsql_status(op, &i_value, &mx_value, reset_flag);
  result = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(rc));
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(i_value));
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(mx_value));
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*
** Usage:    dbsql_db_status  DATABASE  OPCODE  RESETFLAG
**
** Return a list of three elements which are the dbsql_db_status() return
** code, the current value, and the high-water mark value.
*/
static int DBSQL_TCLAPI test_db_status(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc, i_value, mx_value;
  int i, op = 0, reset_flag;
  const char *op_name;
  DBSQL *db;
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  static const struct {
    const char *name;
    int op;
  } ops[] = {
    { "LOOKASIDE_USED",      DBSQL_DBSTATUS_LOOKASIDE_USED      },
    { "CACHE_USED",          DBSQL_DBSTATUS_CACHE_USED          },
    { "SCHEMA_USED",         DBSQL_DBSTATUS_SCHEMA_USED         },
    { "STMT_USED",           DBSQL_DBSTATUS_STMT_USED           },
    { "LOOKASIDE_HIT",       DBSQL_DBSTATUS_LOOKASIDE_HIT       },
    { "LOOKASIDE_MISS_SIZE", DBSQL_DBSTATUS_LOOKASIDE_MISS_SIZE },
    { "LOOKASIDE_MISS_FULL", DBSQL_DBSTATUS_LOOKASIDE_MISS_FULL },
    { "CACHE_HIT",           DBSQL_DBSTATUS_CACHE_HIT           },
    { "CACHE_MISS",          DBSQL_DBSTATUS_CACHE_MISS          },
    { "CACHE_WRITE",         DBSQL_DBSTATUS_CACHE_WRITE         },
    { "DEFERRED_FKS",        DBSQL_DBSTATUS_DEFERRED_FKS        },
    { "CACHE_USED_SHARED",   DBSQL_DBSTATUS_CACHE_USED_SHARED   },
    { "CACHE_SPILL",         DBSQL_DBSTATUS_CACHE_SPILL         },
    { "TEMPBUF_SPILL",       DBSQL_DBSTATUS_TEMPBUF_SPILL       },
  };
  Tcl_Obj *result;
  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB PARAMETER RESETFLAG");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  op_name = Tcl_GetString(objv[2]);
  if( memcmp(op_name, "DBSQL_", 6)==0 ) op_name += 6;
  if( memcmp(op_name, "DBSTATUS_", 9)==0 ) op_name += 9;
  for(i=0; i<ARRAY_SIZE(ops); i++){
    if( strcmp(ops[i].name, op_name)==0 ){
      op = ops[i].op;
      break;
    }
  }
  if( i>=ARRAY_SIZE(ops) ){
    if( Tcl_GetIntFromObj(interp, objv[2], &op) ) return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[3], &reset_flag) ) return TCL_ERROR;
  i_value = 0;
  mx_value = 0;
  rc = dbsql_db_status(db, op, &i_value, &mx_value, reset_flag);
  result = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(rc));
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(i_value));
  Tcl_ListObjAppendElement(0, result, Tcl_NewIntObj(mx_value));
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*
** install_malloc_faultsim BOOLEAN
*/
static int DBSQL_TCLAPI test_install_malloc_faultsim(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int is_install;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[1], &is_install) ){
    return TCL_ERROR;
  }
  rc = __faultsim_install(is_install);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** dbsql_install_memsys3
*/
static int DBSQL_TCLAPI test_install_memsys3(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = DBSQL_MISUSE;
#ifdef DBSQL_ENABLE_MEMSYS3
  const dbsql_mem_methods_t *__mem_get_memsys3(void);
  rc = dbsql_config(DBSQL_CONFIG_MALLOC, __mem_get_memsys3());
#endif
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

static int DBSQL_TCLAPI test_vfs_oom_test(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int dbsql_memdebug_vfs_oom_test;
  if( objc>2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?INTEGER?");
    return TCL_ERROR;
  }else if( objc==2 ){
    int i_new;
    if( Tcl_GetIntFromObj(interp, objv[1], &i_new) ) return TCL_ERROR;
    dbsql_memdebug_vfs_oom_test = i_new;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(dbsql_memdebug_vfs_oom_test));
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest_malloc_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     int clientData;
  } obj_cmds[] = {
     { "dbsql_malloc",             test_malloc                   ,0 },
     { "dbsql_realloc",            test_realloc                  ,0 },
     { "dbsql_free",               test_free                     ,0 },
     { "memset",                     test_memset                   ,0 },
     { "memget",                     test_memget                   ,0 },
     { "dbsql_memory_used",        test_memory_used              ,0 },
     { "dbsql_memory_highwater",   test_memory_highwater         ,0 },
     { "dbsql_memdebug_backtrace", test_memdebug_backtrace       ,0 },
     { "dbsql_memdebug_dump",      test_memdebug_dump            ,0 },
     { "dbsql_memdebug_fail",      test_memdebug_fail            ,0 },
     { "dbsql_memdebug_pending",   test_memdebug_pending         ,0 },
     { "dbsql_memdebug_settitle",  test_memdebug_settitle        ,0 },
     { "dbsql_memdebug_malloc_count", test_memdebug_malloc_count ,0 },
     { "dbsql_memdebug_log",       test_memdebug_log             ,0 },
     { "dbsql_config_pagecache",   test_config_pagecache         ,0 },
     { "dbsql_config_alt_pcache",  test_alt_pcache               ,0 },
     { "dbsql_status",             test_status                   ,0 },
     { "dbsql_db_status",          test_db_status                ,0 },
     { "install_malloc_faultsim",    test_install_malloc_faultsim  ,0 },
     { "dbsql_config_heap",        test_config_heap              ,0 },
     { "dbsql_config_heap_size",   test_config_heap_size         ,0 },
     { "dbsql_config_memstatus",   test_config_memstatus         ,0 },
     { "dbsql_config_lookaside",   test_config_lookaside         ,0 },
     { "dbsql_config_error",       test_config_error             ,0 },
     { "dbsql_config_uri",         test_config_uri               ,0 },
     { "dbsql_config_cis",         test_config_cis               ,0 },
     { "dbsql_config_pmasz",       test_config_pmasz             ,0 },
     { "dbsql_db_config_lookaside",test_db_config_lookaside      ,0 },
     { "dbsql_dump_memsys3",       test_dump_memsys3             ,3 },
     { "dbsql_dump_memsys5",       test_dump_memsys3             ,5 },
     { "dbsql_install_memsys3",    test_install_memsys3          ,0 },
     { "dbsql_memdebug_vfs_oom_test", test_vfs_oom_test          ,0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    ClientData c = (ClientData)DBSQL_INT_TO_PTR(obj_cmds[i].clientData);
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, c, 0);
  }
  return TCL_OK;
}
#endif
