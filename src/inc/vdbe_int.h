/*
** 2003 September 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the header file for information that is private to the
** VDBE.  This information used to all be at the top of the single
** source code file "vdbe.c".  When that file became too big (over
** 6000 lines long) it was split up into several smaller files and
** this header information was factored out.
*/
#ifndef DBSQL_VDBEINT_H
#define DBSQL_VDBEINT_H

/*
** The maximum number of times that a statement will try to reparse
** itself before giving up and returning DBSQL_SCHEMA.
*/
#ifndef DBSQL_MAX_SCHEMA_RETRY
# define DBSQL_MAX_SCHEMA_RETRY 50
#endif

/*
** VDBE_DISPLAY_P4 is true or false depending on whether or not the
** "explain" P4 display logic is enabled.
*/
#if !defined(DBSQL_OMIT_EXPLAIN) || !defined(NDEBUG) \
     || defined(VDBE_PROFILE) || defined(DBSQL_DEBUG) \
     || defined(DBSQL_ENABLE_BYTECODE_VTAB)
# define VDBE_DISPLAY_P4 1
#else
# define VDBE_DISPLAY_P4 0
#endif

/*
** SQL is translated into a sequence of instructions to be
** executed by a virtual machine.  Each instruction is an instance
** of the following structure.
*/
typedef struct vdbe_op_t op_t;

/*
** Boolean values
*/
typedef unsigned Bool;

/* Opaque type used by code in vdbesort.c */
typedef struct vdbe_sorter_t vdbe_sorter_t;

/* Elements of the linked list at vdbe_t.aux_data */
typedef struct aux_data_t aux_data_t;

/* A cache of large TEXT or BLOB values in a vdbe_cursor_t */
typedef struct vdbe_txt_blb_cache_t vdbe_txt_blb_cache_t;

/* Types of VDBE cursors */
#define CURTYPE_BTREE       0
#define CURTYPE_SORTER      1
#define CURTYPE_VTAB        2
#define CURTYPE_PSEUDO      3

/*
** A vdbe_cursor_t is an superclass (a wrapper) for various cursor objects:
**
**      * A b-tree cursor
**          -  In the main database or in an ephemeral database
**          -  On either an index or a table
**      * A sorter
**      * A virtual table
**      * A one-row "pseudotable" stored in a single register
*/
typedef struct vdbe_cursor_t vdbe_cursor_t;
struct vdbe_cursor_t {
  u_int8_t cur_type;            /* One of the CURTYPE_* values above */
  int8_t i_db;                 /* index_t of cursor database in db->dbs[] */
  u_int8_t null_row;             /* True if pointing to a row with no data */
  u_int8_t deferred_moveto;      /* A call to __sm_moveto() is needed */
  u_int8_t is_table;             /* True for rowid tables.  False for indexes */
#ifdef DBSQL_DEBUG
  u_int8_t seek_op;              /* Most recent seek operation on this cursor */
  u_int8_t wr_flag;              /* The wr_flag argument to __sm_cursor() */
#endif
  Bool is_ephemeral:1;     /* True for an ephemeral table */
  Bool use_random_rowid:1;  /* Generate new record numbers semi-randomly */
  Bool is_ordered:1;       /* True if the table is not BTREE_UNORDERED */
  Bool no_reuse:1;         /* __open_ephemeral may not reuse this cursor */
  Bool col_cache:1;        /* cache pointer is initialized and non-NULL */
  u_int16_t seek_hit;            /* See the OP_SeekHit and OP_IfNoHope opcodes */
  union {                 /* btx for is_ephermeral.  alt_map otherwise */
    sm_t *btx;            /* Separate file holding temporary table */
    u_int32_t *alt_maps;           /* Mapping from table to index column numbers */
  } ub;
  int64_t seq_count;           /* Sequence counter */

  /* Cached OP_Column parse information is only valid if cache_status matches
  ** vdbe_t.cache_ctr.  vdbe_t.cache_ctr will never take on the value of
  ** CACHE_STALE (0) and so setting cache_status=CACHE_STALE guarantees that
  ** the cache is out of date. */
  u_int32_t cache_status;        /* Cache is valid if this matches vdbe_t.cache_ctr */
  int seek_result;         /* Result of previous __sm_moveto() or 0
                          ** if there have been no prior seeks on the cursor. */
  /* seek_result does not distinguish between "no seeks have ever occurred
  ** on this cursor" and "the most recent seek was an exact match".
  ** For CURTYPE_PSEUDO, seek_result is the register holding the record */

  /* When a new vdbe_cursor_t is allocated, only the fields above are zeroed.
  ** The fields that follow are uninitialized, and must be individually
  ** initialized prior to first use. */
  vdbe_cursor_t *alt_cursor; /* Associated index cursor from which to read */
  union {
    sm_cursor_t *cursor;          /* CURTYPE_BTREE or _PSEUDO.  sm_t cursor */
    dbsql_vtab_cursor_t *v_cur; /* CURTYPE_VTAB.              Vtab cursor */
    vdbe_sorter_t *sorter;        /* CURTYPE_SORTER.            Sorter object */
  } uc;
  key_info_t *key_info;      /* Info about index keys needed by index cursors */
  u_int32_t i_hdr_offset;         /* Offset to next unparsed byte of the header */
  pgno_t pgno_root;          /* Root page of the open btree cursor */
  int16_t n_field;             /* Number of fields in the header */
  u_int16_t n_hdr_parsed;         /* Number of header fields parsed so far */
  int64_t moveto_target;       /* Argument to the deferred __sm_moveto() */
  u_int32_t *offsets;           /* Pointer to types[n_field] */
  const u_int8_t *rows;         /* Data for the current row, if all on one page */
  u_int32_t payload_size;        /* Total number of bytes in the record */
  u_int32_t sz_row;              /* Byte available in rows */
#ifdef DBSQL_ENABLE_COLUMN_USED_MASK
  u_int64_t mask_used;           /* Mask of columns used by this cursor */
#endif
  vdbe_txt_blb_cache_t *cache; /* Cache of large TEXT or BLOB values */

  /* Space is allocated for types to hold at least 2*n_field+1 entries:
  ** n_field slots for types[] and n_field+1 array slots for offsets[] */
  u_int32_t types[FLEXARRAY];    /* Type values record decode.  MUST BE LAST */
};

/*
** The size (in bytes) of a vdbe_cursor_t object that has an n_field value of N
** or less.  The value of SZ_VDBECURSOR(n) is guaranteed to be a multiple
** of 8.
*/
#define SZ_VDBECURSOR(N) \
    (ROUND8(offsetof(vdbe_cursor_t,types)) + ((N)+1)*sizeof(u_int64_t))

/* Return true if P is a null-only cursor
*/
#define IS_NULL_CURSOR(P) \
  ((P)->cur_type==CURTYPE_PSEUDO && (P)->null_row && (P)->seek_result==0)

/*
** A value for vdbe_cursor_t.cache_status that means the cache is always invalid.
*/
#define CACHE_STALE 0

/*
** Large TEXT or BLOB values can be slow to load, so we want to avoid
** loading them more than once.  For that reason, large TEXT and BLOB values
** can be stored in a cache defined by this object, and attached to the
** vdbe_cursor_t using the cache field.
*/
struct vdbe_txt_blb_cache_t {
  char *c_value;        /* A rc_str_t buffer to hold the value */
  int64_t i_offset;          /* File offset of the row being cached */
  int i_col;             /* column_t for which the cache is valid */
  u_int32_t cache_status;      /* vdbe_t.cache_ctr value */
  u_int32_t col_cache_ctr;      /* column_t cache counter */
};

/*
** When a sub-program is executed (OP_Program), a structure of this type
** is allocated to store the current value of the program counter, as
** well as the current memory cell array and various other frame specific
** values stored in the vdbe_t struct. When the sub-program is finished,
** these values are copied back to the vdbe_t from the vdbe_frame_t structure,
** restoring the state of the VM to as it was before the sub-program
** began executing.
**
** The memory for a vdbe_frame_t object is allocated and managed by a memory
** cell in the parent (calling) frame. When the memory cell is deleted or
** overwritten, the vdbe_frame_t object is not freed immediately. Instead, it
** is linked into the vdbe_t.del_frame list. The contents of the vdbe_t.del_frame
** list is deleted when the VM is reset in __vdbe_halt(). The reason for doing
** this instead of deleting the vdbe_frame_t immediately is to avoid recursive
** calls to __vdbe_mem_release() when the memory cells belonging to the
** child frame are released.
**
** The currently executing frame is stored in vdbe_t.frame. vdbe_t.frame is
** set to NULL if the currently executing frame is the main program.
*/
typedef struct vdbe_frame_t vdbe_frame_t;
struct vdbe_frame_t {
  vdbe_t *v;                /* VM this frame belongs to */
  vdbe_frame_t *parent;     /* Parent of this frame, or NULL if parent is main */
  op_t *ops;                /* Program instructions for parent frame */
  mem_t *mems;              /* Array of memory cells for parent frame */
  vdbe_cursor_t **csrs;     /* Array of vdbe_t cursors for parent frame */
  u_int8_t *onces;              /* Bitmask used by OP_Once */
  void *token;            /* Copy of sub_program_t.token */
  int64_t last_rowid;          /* Last insert rowid (DBSQL.last_rowid) */
  aux_data_t *aux_data;      /* Linked list of auxdata allocations */
#if DBSQL_DEBUG
  u_int32_t i_frame_magic;        /* magic number for sanity checking */
#endif
  int n_cursor;            /* Number of entries in csrs */
  int pc;                 /* Program Counter in parent (calling) frame */
  int n_op;                /* Size of ops array */
  int n_mem;               /* Number of entries in mems */
  int n_child_mem;          /* Number of memory cells for child frame */
  int n_child_csr;          /* Number of cursors for child frame */
  int64_t n_change;            /* Statement changes (vdbe_t.n_change)     */
  int64_t n_db_change;          /* Value of db->n_change */
};

/* Magic number for sanity checking on vdbe_frame_t objects */
#define DBSQL_FRAME_MAGIC 0x879fb71e

/*
** Return a pointer to the array of registers allocated for use
** by a vdbe_frame_t.
*/
#define VDBE_FRAME_MEM(p) ((mem_t *)&((u_int8_t *)p)[ROUND8(sizeof(vdbe_frame_t))])

/*
** Internally, the vdbe manipulates nearly all SQL values as mem_t
** structures. Each mem_t struct may cache multiple representations (string,
** integer etc.) of the same value.
*/
struct dbsql_value_t {
  union mem_value_t {
    double r;           /* Real value used when MEM_Real is set in flags */
    int64_t i;              /* Integer value used when MEM_Int is set in flags */
    int n_zero;          /* Extra zero bytes when MEM_Zero and MEM_Blob set */
    const char *p_type; /* Pointer type when MEM_Term|MEM_Subtype|MEM_Null */
    func_def_t *def;      /* Used only when flags==MEM_Agg */
  } u;
  char *z;            /* String or BLOB value */
  int n;              /* Number of characters in string value, excluding '\0' */
  u_int16_t flags;          /* Some combination of MEM_Null, MEM_Str, MEM_Dyn, etc. */
  u_int8_t  enc;            /* DBSQL_UTF8, DBSQL_UTF16BE, DBSQL_UTF16LE */
  u_int8_t  subtype;       /* Subtype for this value */
  /* __shallow_copy only needs to copy the information above */
  DBSQL *db;        /* The associated database connection */
  int sz_malloc;       /* Size of the z_malloc allocation */
  u_int32_t u_temp;          /* Transient storage for serial_type in OP_MakeRecord */
  char *z_malloc;      /* Space to hold MEM_Str or MEM_Blob if sz_malloc>0 */
  void (*x_del)(void*);/* Destructor for mem_t.z - only valid if MEM_Dyn */
#ifdef DBSQL_DEBUG
  mem_t *scopy_from;    /* This mem_t is a shallow copy of scopy_from */
  u_int16_t m_scopy_flags;    /* flags value immediately after the shallow copy */
  u_int8_t  scopy;         /* The scopy_from of some other mem_t *might* point here */
#endif
};

/*
** Size of struct mem_t not including the mem_t.z_malloc member or anything that
** follows.
*/
#define MEMCELLSIZE offsetof(mem_t,db)

/* One or more of the following flags are set to indicate the
** representations of the value stored in the mem_t struct.
**
**  *  MEM_Null                An SQL NULL value
**
**  *  MEM_Null|MEM_Zero       An SQL NULL with the virtual table
**                             UPDATE no-change flag set
**
**  *  MEM_Null|MEM_Term|      An SQL NULL, but also contains a
**        MEM_Subtype          pointer accessible using
**                             dbsql_value_pointer().
**
**  *  MEM_Null|MEM_Cleared    Special SQL NULL that compares non-equal
**                             to other NULLs even using the IS operator.
**
**  *  MEM_Str                 A string, stored in mem_t.z with
**                             length mem_t.n.  Zero-terminated if
**                             MEM_Term is set.  This flag is
**                             incompatible with MEM_Blob and
**                             MEM_Null, but can appear with MEM_Int,
**                             MEM_Real, and MEM_IntReal.
**
**  *  MEM_Blob                A blob, stored in mem_t.z length mem_t.n.
**                             Incompatible with MEM_Str, MEM_Null,
**                             MEM_Int, MEM_Real, and MEM_IntReal.
**
**  *  MEM_Blob|MEM_Zero       A blob in mem_t.z of length mem_t.n plus
**                             mem_t.u.n_zero extra 0x00 bytes at the end.
**
**  *  MEM_Int                 Integer stored in mem_t.u.i.
**
**  *  MEM_Real                Real stored in mem_t.u.r.
**
**  *  MEM_IntReal             Real stored as an integer in mem_t.u.i.
**
** If the MEM_Null flag is set, then the value is an SQL NULL value.
** For a pointer type created using dbsql_bind_pointer() or
** dbsql_result_pointer() the MEM_Term and MEM_Subtype flags are also set.
**
** If the MEM_Str flag is set then mem_t.z points at a string representation.
** Usually this is encoded in the same unicode encoding as the main
** database (see below for exceptions). If the MEM_Term flag is also
** set, then the string is nul terminated. The MEM_Int and MEM_Real
** flags may coexist with the MEM_Str flag.
*/
#define MEM_Undefined 0x0000   /* Value is undefined */
#define MEM_Null      0x0001   /* Value is NULL (or a pointer) */
#define MEM_Str       0x0002   /* Value is a string */
#define MEM_Int       0x0004   /* Value is an integer */
#define MEM_Real      0x0008   /* Value is a real number */
#define MEM_Blob      0x0010   /* Value is a BLOB */
#define MEM_IntReal   0x0020   /* MEM_Int that stringifies like MEM_Real */
#define MEM_AffMask   0x003f   /* Mask of affinity bits */

/* Extra bits that modify the meanings of the core datatypes above
*/
#define MEM_FromBind  0x0040   /* Value originates from dbsql_bind() */
 /*                   0x0080   // Available */
#define MEM_Cleared   0x0100   /* NULL set by OP_Null, not from data */
#define MEM_Term      0x0200   /* String in mem_t.z is zero terminated */
#define MEM_Zero      0x0400   /* mem_t.i contains count of 0s appended to blob */
#define MEM_Subtype   0x0800   /* mem_t.subtype is valid */
#define MEM_TypeMask  0x0dbf   /* Mask of type bits */

/* Bits that determine the storage for mem_t.z for a string or blob or
** aggregate accumulator.
*/
#define MEM_Dyn       0x1000   /* Need to call mem_t.del() on mem_t.z */
#define MEM_Static    0x2000   /* mem_t.z points to a static string */
#define MEM_Ephem     0x4000   /* mem_t.z points to an ephemeral string */
#define MEM_Agg       0x8000   /* mem_t.z points to an agg function context */

/* Return TRUE if mem_t X contains dynamically allocated content - anything
** that needs to be deallocated to avoid a leak.
*/
#define VDBE_MEM_DYNAMIC(X)  \
  (((X)->flags&(MEM_Agg|MEM_Dyn))!=0)

/*
** Clear any existing type flags from a mem_t and replace them with f
*/
#define MEM_SET_TYPE_FLAG(p, f) \
   ((p)->flags = ((p)->flags&~(MEM_TypeMask|MEM_Zero))|f)

/*
** True if mem_t X is a NULL-nochng type.
*/
#define MEM_NULL_NOCHNG(X) \
  (((X)->flags&MEM_TypeMask)==(MEM_Null|MEM_Zero) \
    && (X)->n==0 && (X)->u.n_zero==0)

/*
** Return true if a memory cell has been initialized and is valid.
** is for use inside assert() statements only.
**
** A Memory cell is initialized if at least one of the
** MEM_Null, MEM_Str, MEM_Int, MEM_Real, MEM_Blob, or MEM_IntReal bits
** is set.  It is "undefined" if all those bits are zero.
*/
#ifdef DBSQL_DEBUG
#define MEM_IS_VALID(M)  ((M)->flags & MEM_AffMask)!=0
#endif

/*
** Each auxiliary data pointer stored by a user defined function
** implementation calling dbsql_set_auxdata() is stored in an instance
** of this structure. All such structures associated with a single VM
** are stored in a linked list headed at vdbe_t.aux_data. All are destroyed
** when the VM is halted (if not before).
*/
struct aux_data_t {
  int i_aux_op;                     /* Instruction number of OP_Function opcode */
  int i_aux_arg;                    /* index_t of function argument. */
  void *aux;                     /* Aux data pointer */
  void (*delete_aux)(void*);      /* Destructor for the aux data */
  aux_data_t *next_aux;              /* Next element in list */
};

/*
** The "context" argument for an installable function.  A pointer to an
** instance of this structure is the first argument to the routines used
** implement the SQL functions.
**
** There is a typedef for this structure in sqlite.h.  So all routines,
** even the public interface to dbsql, can use a pointer to this structure.
** But this file is the only place where the internal details of this
** structure are known.
**
** This structure is defined inside of vdbe_int.h because it uses substructures
** (mem_t) which are only defined there.
*/
struct dbsql_func_t {
  mem_t *out;              /* The return value is stored here */
  func_def_t *func;         /* Pointer to function information */
  mem_t *mem;              /* Memory cell used to store aggregate context */
  vdbe_t *vdbe;            /* The VM that owns this context */
  int i_op;                /* Instruction number of OP_Function */
  int isError;            /* Error code returned by the function. */
  u_int8_t enc;                 /* Encoding to use for results */
  u_int8_t skip_flag;            /* Skip accumulator loading if true */
  u_int16_t argc;               /* Number of arguments */
  dbsql_value_t *argv[FLEXARRAY]; /* Argument set */
};

/*
** The size (in bytes) of an dbsql_func_t object that holds N
** argv[] arguments.
*/
#define SZ_CONTEXT(N)  \
   (offsetof(dbsql_func_t,argv)+(N)*sizeof(dbsql_value_t*))


/* The scan_status_t object holds a single value for the
** dbsql_stmt_scanstatus() interface.
**
** addr_ranges[]:
**   This array is used by scan_status_t elements associated with EQP
**   notes that make an DBSQL_SCANSTAT_NCYCLE value available. It is
**   an array of up to 3 ranges of VM addresses for which the vdbe_t.an_cycle[]
**   values should be summed to calculate the NCYCLE value. Each pair of
**   integer addresses is a start and end address (both inclusive) for a range
**   instructions. A start value of 0 indicates an empty range.
*/
typedef struct scan_status_t scan_status_t;
struct scan_status_t {
  int addr_explain;                /* OP_Explain for loop */
  int addr_ranges[6];
  int addr_loop;                   /* Address of "loops" counter */
  int addr_visit;                  /* Address of "rows visited" counter */
  int i_select_id;                  /* The "select_t-ID" for this loop */
  log_est_t n_est;                    /* Estimated output rows per loop */
  char *name;                    /* Name of table or index */
};

/* The dblquote_str_t object holds the text of a double-quoted
** string for a prepared statement.  A linked list of these objects
** is constructed during statement parsing and is held on vdbe_t.dbl_str.
** When computing a normalized SQL statement for an SQL statement, that
** list is consulted for each double-quoted identifier to see if the
** identifier should really be a string literal.
*/
typedef struct dblquote_str_t dblquote_str_t;
struct dblquote_str_t {
  dblquote_str_t *next_str;   /* Next string literal in the list */
  char z[8];               /* Dequoted value for the string */
};

/*
** An instance of the virtual machine.  This structure contains the complete
** state of the virtual machine.
**
** The "dbsql_stmt_t" structure pointer that is returned by dbsql_prepare()
** is really a pointer to an instance of this structure.
*/
struct vdbe_t {
  DBSQL *db;            /* The database connection that owns this statement */
  vdbe_t **v_prev,*v_next; /* Linked list of VDBEs with the same vdbe_t.db */
  parser_t *parser;          /* Parsing context used to create this vdbe_t */
  yn_var_t n_var;             /* Number of entries in vars[] */
  int n_mem;               /* Number of memory locations currently allocated */
  int n_cursor;            /* Number of slots in csrs[] */
  u_int32_t cache_ctr;           /* vdbe_cursor_t row cache generation counter */
  int pc;                 /* The program counter */
  int rc;                 /* Value to return */
  int64_t n_change;            /* Number of db changes made since last reset */
  int i_statement;         /* Statement number (or 0 if has no opened stmt) */
  int64_t i_current_time;       /* Value of julianday('now') for this statement */
  int64_t n_fk_constraint;      /* Number of imm. FK constraints this VM */
  int64_t n_stmt_def_cons;       /* Number of def. constraints when stmt started */
  int64_t n_stmt_def_imm_cons;    /* Number of def. imm constraints when stmt started */
  mem_t *mems;              /* The memory locations */
  mem_t **args;            /* Arguments update and filter vtab methods */
  vdbe_cursor_t **csrs;     /* One element of this array for each open cursor */
  mem_t *vars;              /* Values for the OP_Variable opcode. */

  /* When allocating a new vdbe_t object, all of the fields below should be
  ** initialized to zero or NULL */

  op_t *ops;                /* Space to hold the virtual machine's program */
  int n_op;                /* Number of instructions in the program */
  int n_op_alloc;           /* Slots allocated for ops[] */
  mem_t *col_names;          /* column_t names to return */
  mem_t *result_row;        /* Current output row */
  char *errmsg;          /* Error message written here */
  vlist_t *v_list;          /* Name of variables */
#ifndef DBSQL_OMIT_TRACE
  int64_t startTime;          /* Time when query started - used for profiling */
#endif
#ifdef DBSQL_DEBUG
  int rc_app;              /* errcode set by dbsql_result_error_code() */
  u_int32_t n_write;             /* Number of write operations that have occurred */
  int nap_arg;             /* Size of the args[] array */
#endif
  u_int16_t n_res_column;         /* Number of columns in one row of the result set */
  u_int16_t n_res_alloc;          /* column_t slots allocated to col_names[] */
  u_int8_t error_action;         /* Recovery action to do in case of an error */
  u_int8_t min_write_file_format;  /* Minimum file format for writable database files */
  u_int8_t prep_flags;           /* DBSQL_PREPARE_* flags */
  u_int8_t vdbe_state;          /* On of the VDBE_*_STATE values */
  bft_t expired:2;          /* 1: recompile VM immediately  2: when convenient */
  bft_t explain:2;          /* 0: normal, 1: EXPLAIN, 2: EXPLAIN QUERY PLAN */
  bft_t change_cnt_on:1;      /* True to update the change-counter */
  bft_t uses_stmt_journal:1;  /* True if uses a statement journal */
  bft_t readOnly:1;         /* True for statements that do not write */
  bft_t is_reader:1;        /* True for statements that read */
  bft_t have_eqp_ops:1;       /* Bytecode supports EXPLAIN QUERY PLAN */
  db_mask_t btree_mask;      /* Bitmask of db->dbs[] entries referenced */
  db_mask_t lock_mask;       /* Subset of btree_mask that requires a lock */
  u_int32_t counters[9];        /* Counters used by dbsql_stmt_status() */
  char *z_sql;             /* Text of the SQL statement that generated this */
#ifdef DBSQL_ENABLE_NORMALIZE
  char *norm_sql;         /* Normalization of the associated SQL statement */
  dblquote_str_t *dbl_str;   /* List of double-quoted string literals */
#endif
  void *p_free;            /* Free this when deleting the vdbe */
  vdbe_frame_t *frame;      /* Parent frame */
  vdbe_frame_t *del_frame;   /* List of frame objects to free on VM reset */
  int n_frame;             /* Number of frames in frame list */
  u_int32_t expmask;            /* Binding to these vars invalidates VM */
  sub_program_t *program;   /* Linked list of all sub-programs used by VM */
  aux_data_t *aux_data;      /* Linked list of auxdata allocations */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int n_scan;              /* Entries in scans[] */
  scan_status_t *scans;      /* Scan definitions for dbsql_stmt_scanstatus() */
#endif
};

/*
** The following are allowed values for vdbe_t.vdbe_state
*/
#define VDBE_INIT_STATE     0   /* Prepared statement under construction */
#define VDBE_READY_STATE    1   /* Ready to run but not yet started */
#define VDBE_RUN_STATE      2   /* Run in progress */
#define VDBE_HALT_STATE     3   /* Finished.  Need reset() or finalize() */

/*
** Structure used to store the context required by the
** dbsql_preupdate_*() API functions.
*/
struct pre_update_t {
  vdbe_t *v;
  vdbe_cursor_t *csr;               /* Cursor to read old values from */
  int op;                         /* One of DBSQL_INSERT, UPDATE, DELETE */
  u_int8_t *records;                    /* old.* database record */
  key_info_t *keyinfo;              /* Key information */
  unpacked_record_t *unpacked;      /* Unpacked version of records[] */
  unpacked_record_t *new_unpacked;   /* Unpacked version of new.* record */
  int i_new_reg;                    /* Register for new.* values */
  int i_blob_write;                 /* Value returned by preupdate_blobwrite() */
  int64_t i_key1;                      /* First key value passed to hook */
  int64_t i_key2;                      /* Second key value passed to hook */
  mem_t oldipk;                     /* Memory cell holding "old" IPK value */
  mem_t *news;                      /* Array of new.* values */
  table_t *tab;                    /* schema_t object being updated */
  index_t *pk;                     /* PK index if tab is WITHOUT ROWID */
  dbsql_value_t **dflts;         /* Array of default values, if required */
  struct {
    u_int8_t keyinfo_space[SZ_KEYINFO_0];  /* Space to hold keyinfo[0] content */
  } u_key;
};

/*
** An instance of this object is used to pass an vector of values into
** OP_VFilter, the filter method of a virtual table.  The vector is the
** set of values on the right-hand side of an IN constraint.
**
** The value as passed into filter is an dbsql_value_t with a "pointer"
** type, such as is generated by dbsql_result_pointer() and read by
** dbsql_value_pointer.  Such values have MEM_Term|MEM_Subtype|MEM_Null
** and a subtype of 'p'.  The dbsql_vtab_in_first() and _next() interfaces
** know how to use this object to step through all the values in the
** right operand of the IN constraint.
*/
typedef struct value_list_t value_list_t;
struct value_list_t {
  sm_cursor_t *csr;          /* An ephemeral table holding all values */
  dbsql_value_t *out;     /* Register to hold each decoded output value */
};

/* Size of content associated with serial types that fit into a
** single-byte varint.
*/
#ifndef DBSQL_AMALGAMATION
extern const u_int8_t __small_type_sizes[];
#endif

/*
** Function prototypes
*/
void __vdbe_error(vdbe_t*, const char *, ...);
void __vdbe_free_cursor(vdbe_t *, vdbe_cursor_t*);
void __vdbe_free_cursor_nn(vdbe_t*,vdbe_cursor_t*);
void dbsql_vdbe_pop_stack(vdbe_t*,int);
int DBSQL_NOINLINE __vdbe_handle_moved_cursor(vdbe_cursor_t *p);
int DBSQL_NOINLINE __vdbe_finish_moveto(vdbe_cursor_t*);
int __vdbe_cursor_restore(vdbe_cursor_t*);
u_int32_t __vdbe_serial_type_len(u_int32_t);
u_int8_t __vdbe_one_byte_serial_type_len(u_int8_t);
#ifdef DBSQL_MIXED_ENDIAN_64BIT_FLOAT
  u_int64_t __float_swap(u_int64_t in);
# define SWAP_MIXED_ENDIAN_FLOAT(X)  X = __float_swap(X)
#else
# define SWAP_MIXED_ENDIAN_FLOAT(X)
#endif
void __vdbe_serial_get(const unsigned char*, u_int32_t, mem_t*);
void __vdbe_delete_aux_data(DBSQL*, aux_data_t**, int, int);

int dbsql2_btree_key_compare(sm_cursor_t *, const void *, int, int, int *);
int __vdbe_idx_key_compare(DBSQL*,vdbe_cursor_t*,unpacked_record_t*,int*);
int __vdbe_idx_rowid(DBSQL*, sm_cursor_t*, int64_t*);
int __vdbe_exec(vdbe_t*);
#if !defined(DBSQL_OMIT_EXPLAIN) || defined(DBSQL_ENABLE_BYTECODE_VTAB)
int __vdbe_next_opcode(vdbe_t*,mem_t*,int,int*,int*,op_t**);
char *__vdbe_display_p4(DBSQL*,op_t*);
#endif
#if defined(DBSQL_ENABLE_EXPLAIN_COMMENTS)
char *__vdbe_display_comment(DBSQL*,const op_t*,const char*);
#endif
#if !defined(DBSQL_OMIT_EXPLAIN)
int __vdbe_list(vdbe_t*);
#endif
int __vdbe_halt(vdbe_t*);
int __vdbe_change_encoding(mem_t *, int);
int __vdbe_mem_too_big(mem_t*);
int __vdbe_mem_copy(mem_t*, const mem_t*);
void __vdbe_mem_shallow_copy(mem_t*, const mem_t*, int);
void __vdbe_mem_move(mem_t*, mem_t*);
int __vdbe_mem_nul_terminate(mem_t*);
int __vdbe_mem_set_str(mem_t*, const char*, int64_t, u_int8_t, void(*)(void*));
int __vdbe_mem_set_text(mem_t*, const char*, int64_t, void(*)(void*));
void __vdbe_mem_set_int64(mem_t*, int64_t);
#ifdef DBSQL_OMIT_FLOATING_POINT
# define __vdbe_mem_set_double __vdbe_mem_set_int64
#else
  void __vdbe_mem_set_double(mem_t*, double);
#endif
void __vdbe_mem_set_pointer(mem_t*, void*, const char*, void(*)(void*));
void __vdbe_mem_init(mem_t*,DBSQL*,u_int16_t);
void __vdbe_mem_set_null(mem_t*);
#ifndef DBSQL_OMIT_INCRBLOB
void __vdbe_mem_set_zero_blob(mem_t*,int);
#else
int __vdbe_mem_set_zero_blob(mem_t*,int);
#endif
#ifdef DBSQL_DEBUG
int __vdbe_mem_is_row_set(const mem_t*);
#endif
int __vdbe_mem_set_row_set(mem_t*);
int __vdbe_mem_zero_terminate_if_able(mem_t*);
int __vdbe_mem_make_writeable(mem_t*);
int __vdbe_mem_stringify(mem_t*, u_int8_t, u_int8_t);
int __int_float_compare(int64_t,double);
int64_t __vdbe_int_value(const mem_t*);
int __vdbe_mem_integerify(mem_t*);
double __vdbe_real_value(mem_t*);
int __mem_real_value_rc(mem_t*, double*);
int __vdbe_boolean_value(mem_t*, int if_null);
void __vdbe_integer_affinity(mem_t*);
int __vdbe_mem_realify(mem_t*);
int __vdbe_mem_numerify(mem_t*);
int __vdbe_mem_cast(mem_t*,u_int8_t,u_int8_t);
int __vdbe_mem_from_btree(sm_cursor_t*,u_int32_t,u_int32_t,mem_t*);
int __vdbe_mem_from_btree_zero_offset(sm_cursor_t*,u_int32_t,mem_t*);
void __vdbe_mem_release(mem_t *p);
void __vdbe_mem_release_malloc(mem_t*p);
int __vdbe_mem_finalize(mem_t*, func_def_t*);
#ifndef DBSQL_OMIT_WINDOWFUNC
int __vdbe_mem_agg_value(mem_t*, mem_t*, func_def_t*);
#endif
#if !defined(DBSQL_OMIT_EXPLAIN) || defined(DBSQL_ENABLE_BYTECODE_VTAB)
const char *__opcode_name(int);
#endif
int __vdbe_mem_grow(mem_t *mem, int n, int preserve);
int __vdbe_mem_clear_and_resize(mem_t *mem, int n);
int __vdbe_close_statement(vdbe_t *, int);
#ifdef DBSQL_DEBUG
int __vdbe_frame_is_valid(vdbe_frame_t*);
#endif
void __vdbe_frame_mem_del(void*);      /* Destructor on mem_t */
void __vdbe_frame_delete(vdbe_frame_t*); /* Actually deletes the Frame */
int __vdbe_frame_restore(vdbe_frame_t *);
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
void __vdbe_pre_update_hook(
    vdbe_t*,vdbe_cursor_t*,int,const char*,table_t*,int64_t,int,int);
#endif
int __vdbe_transfer_error(vdbe_t *p);
int __vdbe_find_index_key(sm_cursor_t*, index_t*, unpacked_record_t*, int*, int);

int __vdbe_sorter_init(DBSQL *, int, vdbe_cursor_t *);
void __vdbe_sorter_reset(DBSQL *, vdbe_sorter_t *);
void __vdbe_sorter_close(DBSQL *, vdbe_cursor_t *);
int __vdbe_sorter_rowkey(const vdbe_cursor_t *, mem_t *);
int __vdbe_sorter_next(DBSQL *, const vdbe_cursor_t *);
int __vdbe_sorter_rewind(const vdbe_cursor_t *, int *);
int __vdbe_sorter_write(const vdbe_cursor_t *, mem_t *);
int __vdbe_sorter_compare(const vdbe_cursor_t *, mem_t *, int, int *);

void __vdbe_value_list_free(void*);

#ifdef DBSQL_DEBUG
  void __vdbe_incr_write_counter(vdbe_t*, vdbe_cursor_t*);
  void __vdbe_assert_abortable(vdbe_t*);
#else
# define __vdbe_incr_write_counter(V,C)
# define __vdbe_assert_abortable(V)
#endif

#if !defined(DBSQL_OMIT_SHARED_CACHE)
  void __vdbe_enter(vdbe_t*);
#else
# define __vdbe_enter(X)
#endif

#if !defined(DBSQL_OMIT_SHARED_CACHE) && DBSQL_THREADSAFE>0
  void __vdbe_leave(vdbe_t*);
#else
# define __vdbe_leave(X)
#endif

#ifdef DBSQL_DEBUG
void __vdbe_mem_about_to_change(vdbe_t*,mem_t*);
int __vdbe_check_mem_invariants(mem_t*);
#endif

#ifndef DBSQL_OMIT_FOREIGN_KEY
int __vdbe_check_fk_immediate(vdbe_t*);
int __vdbe_check_fk_deferred(vdbe_t*);
#else
# define __vdbe_check_fk_immediate(p) 0
# define __vdbe_check_fk_deferred(p) 0
#endif

#ifdef DBSQL_DEBUG
  void __vdbe_print_sql(vdbe_t*);
  void __vdbe_mem_pretty_print(mem_t *mem, str_accum_t *str);
#endif
#ifndef DBSQL_OMIT_UTF16
  int __vdbe_mem_translate(mem_t*, u_int8_t);
  int __vdbe_mem_handle_bom(mem_t *mem);
#endif

#ifndef DBSQL_OMIT_INCRBLOB
  int __vdbe_mem_expand_blob(mem_t *);
  #define EXPAND_BLOB(P) (((P)->flags&MEM_Zero)?__vdbe_mem_expand_blob(P):0)
#else
  #define __vdbe_mem_expand_blob(x) DBSQL_OK
  #define EXPAND_BLOB(P) DBSQL_OK
#endif

#endif /* !defined(DBSQL_VDBEINT_H) */
