/*
** 2008 June 13
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
** This file contains definitions of global variables and constants.
*/
#include "dbsql_int.h"

/* An array to map all upper-case characters into their corresponding
** lower-case character. 
**
** dbsql only considers US-ASCII (or EBCDIC) characters.  We do not
** handle case conversions for the UTF character set since the tables
** involved are nearly as big or bigger than dbsql itself.
*/
const unsigned char __upper_to_lower[] = {
#ifdef DBSQL_ASCII
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
     18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
     36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
     54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 97, 98, 99,100,101,102,103,
    104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,
    122, 91, 92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,
    108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
    126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,
    162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,
    180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,
    198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,
    216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,
    234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,
    252,253,254,255,
#endif
#ifdef DBSQL_EBCDIC
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, /* 0x */
     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, /* 1x */
     32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, /* 2x */
     48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, /* 3x */
     64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, /* 4x */
     80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, /* 5x */
     96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111, /* 6x */
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127, /* 7x */
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143, /* 8x */
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159, /* 9x */
    160,161,162,163,164,165,166,167,168,169,170,171,140,141,142,175, /* Ax */
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191, /* Bx */
    192,129,130,131,132,133,134,135,136,137,202,203,204,205,206,207, /* Cx */
    208,145,146,147,148,149,150,151,152,153,218,219,220,221,222,223, /* Dx */
    224,225,162,163,164,165,166,167,168,169,234,235,236,237,238,239, /* Ex */
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255, /* Fx */
#endif
/* All of the upper-to-lower conversion data is above.  The following
** 18 integers are completely unrelated.  They are appended to the
** __upper_to_lower[] array to avoid UBSAN warnings.  Here's what is
** going on:
**
** The SQL comparison operators (<>, =, >, <=, <, and >=) are implemented
** by invoking __mem_compare(A,B) which compares values A and B and
** returns negative, zero, or positive if A is less then, equal to, or
** greater than B, respectively.  Then the true false results is found by
** consulting __dbsqlaLTb[opcode], __dbsqlaEQb[opcode], or 
** __dbsqlaGTb[opcode] depending on whether the result of compare(A,B)
** is negative, zero, or positive, where opcode is the specific opcode.
** The only works because the comparison opcodes are consecutive and in
** this order: NE EQ GT LE LT GE.  Various assert()s throughout the code
** ensure that is the case.
**
** These elements must be appended to another array.  Otherwise the
** index (here shown as [256-OP_Ne]) would be out-of-bounds and thus
** be undefined behavior.  That's goofy, but the C-standards people thought
** it was a good idea, so here we are.
*/
/* NE  EQ  GT  LE  LT  GE  */
   1,  0,  0,  1,  1,  0,  /* l_tbs[]: Use when compare(A,B) less than zero */
   0,  1,  0,  1,  0,  1,  /* e_qbs[]: Use when compare(A,B) equals zero */
   1,  0,  1,  0,  0,  1   /* g_tbs[]: Use when compare(A,B) greater than zero*/
};
const unsigned char *__dbsqlaLTb = &__upper_to_lower[256-OP_Ne];
const unsigned char *__dbsqlaEQb = &__upper_to_lower[256+6-OP_Ne];
const unsigned char *__dbsqlaGTb = &__upper_to_lower[256+12-OP_Ne];

/*
** The following 256 byte lookup table is used to support SQLites built-in
** equivalents to the following standard library functions:
**
**   isspace()                        0x01
**   isalpha()                        0x02
**   isdigit()                        0x04
**   isalnum()                        0x06
**   isxdigit()                       0x08
**   toupper()                        0x20
**   dbsql identifier character      0x40   $, _, or non-ascii
**   Quote character                  0x80
**
** Bit 0x20 is set if the mapped character requires translation to upper
** case. i.e. if the character is a lower-case ASCII character.
** If x is a lower-case ASCII character, then its upper-case equivalent
** is (x - 0x20). Therefore toupper() can be implemented as:
**
**   (x & ~(map[x]&0x20))
**
** The equivalent of tolower() is implemented using the __upper_to_lower[]
** array. tolower() is used more often than toupper() by dbsql.
**
** Bit 0x40 is set if the character is non-alphanumeric and can be used in an 
** dbsql identifier.  Identifiers are alphanumerics, "_", "$", and any
** non-ASCII UTF character. Hence the test for whether or not a character is
** part of an identifier is 0x46.
*/
const unsigned char __ctype_map[256] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 00..07    ........ */
  0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,  /* 08..0f    ........ */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 10..17    ........ */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 18..1f    ........ */
  0x01, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x80,  /* 20..27     !"#$%&' */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 28..2f    ()*+,-./ */
  0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,  /* 30..37    01234567 */
  0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 38..3f    89:;<=>? */

  0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02,  /* 40..47    @ABCDEFG */
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 48..4f    HIJKLMNO */
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 50..57    PQRSTUVW */
  0x02, 0x02, 0x02, 0x80, 0x00, 0x00, 0x00, 0x40,  /* 58..5f    XYZ[\]^_ */
  0x80, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22,  /* 60..67    `abcdefg */
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,  /* 68..6f    hijklmno */
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,  /* 70..77    pqrstuvw */
  0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 78..7f    xyz{|}~. */

  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 80..87    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 88..8f    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 90..97    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 98..9f    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* a0..a7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* a8..af    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* b0..b7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* b8..bf    ........ */

  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* c0..c7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* c8..cf    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* d0..d7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* d8..df    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* e0..e7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* e8..ef    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* f0..f7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40   /* f8..ff    ........ */
};

/* EVIDENCE-OF: R-02982-34736 In order to maintain full backwards
** compatibility for legacy applications, the URI filename capability is
** disabled by default.
**
** EVIDENCE-OF: R-38799-08373 URI filenames can be enabled or disabled
** using the DBSQL_USE_URI=1 or DBSQL_USE_URI=0 compile-time options.
**
** EVIDENCE-OF: R-43642-56306 By default, URI handling is globally
** disabled. The default value may be changed by compiling with the
** DBSQL_USE_URI symbol defined.
*/
#ifndef DBSQL_USE_URI
# define DBSQL_USE_URI 0
#endif

/* EVIDENCE-OF: R-38720-18127 The default setting is determined by the
** DBSQL_ALLOW_COVERING_INDEX_SCAN compile-time option, or is "on" if
** that compile-time option is omitted.
*/
#if !defined(DBSQL_ALLOW_COVERING_INDEX_SCAN)
# define DBSQL_ALLOW_COVERING_INDEX_SCAN 1
#else
# if !DBSQL_ALLOW_COVERING_INDEX_SCAN 
#   error "Compile-time disabling of covering index scan using the\
 -DDBSQL_ALLOW_COVERING_INDEX_SCAN=0 option is deprecated.\
 Contact dbsql developers if this is a problem for you, and\
 delete this #error macro to continue with your build."
# endif
#endif

/* The minimum PMA size is set to this value multiplied by the database
** page size in bytes.
*/
#ifndef DBSQL_SORTER_PMASZ
# define DBSQL_SORTER_PMASZ 250
#endif

/* Statement journals spill to disk when their size exceeds the following
** threshold (in bytes). 0 means that statement journals are created and
** written to disk immediately (the default behavior for dbsql versions
** before 3.12.0).  -1 means always keep the entire statement journal in
** memory.  (The statement journal is also always held entirely in memory
** if journal_mode=MEMORY or if temp_store=MEMORY, regardless of this
** setting.)
*/
#ifndef DBSQL_STMTJRNL_SPILL 
# define DBSQL_STMTJRNL_SPILL (64*1024)
#endif

/*
** The default lookaside-configuration, the format "SZ,N".  SZ is the
** number of bytes in each lookaside slot (should be a multiple of 8)
** and N is the number of slots.  The lookaside-configuration can be
** changed as start-time using dbsql_config(DBSQL_CONFIG_LOOKASIDE)
** or at run-time for an individual database connection using
** dbsql_db_config(db, DBSQL_DBCONFIG_LOOKASIDE);
**
** with_t the two-size-lookaside enhancement, less lookaside is required.
** The default configuration of 1200,40 actually provides 30 1200-byte slots
** and 93 128-byte slots, which is more lookaside than is available
** using the older 1200,100 configuration without two-size-lookaside.
*/
#ifndef DBSQL_DEFAULT_LOOKASIDE
# ifdef DBSQL_OMIT_TWOSIZE_LOOKASIDE
#   define DBSQL_DEFAULT_LOOKASIDE 1200,100  /* 120KB of memory */
# else
#   define DBSQL_DEFAULT_LOOKASIDE 1200,40   /* 48KB of memory */
# endif
#endif


/* The default maximum size of an in-memory database created using
** dbsql_deserialize()
*/
#ifndef DBSQL_MEMDB_DEFAULT_MAXSIZE
# define DBSQL_MEMDB_DEFAULT_MAXSIZE 1073741824
#endif

/*
** The following singleton contains the global configuration for
** the dbsql library.
*/
DBSQL_WSD struct dbsql_config __config = {
   DBSQL_DEFAULT_MEMSTATUS,  /* memstat */
   1,                         /* core_mutex */
   DBSQL_THREADSAFE==1,      /* full_mutex */
   DBSQL_USE_URI,            /* open_uri */
   DBSQL_ALLOW_COVERING_INDEX_SCAN,   /* use_cis */
   0,                         /* small_malloc */
   1,                         /* extra_schema_checks */
#ifdef DBSQL_DEBUG
   0,                         /* json_selfcheck */
#endif
   0x7ffffffe,                /* mx_strlen */
   0,                         /* never_corrupt */
   DBSQL_DEFAULT_LOOKASIDE,  /* sz_lookaside, n_lookaside */
   DBSQL_STMTJRNL_SPILL,     /* n_stmt_spill */
   {0,0,0,0,0,0,0,0},         /* m */
   {0,0,0,0,0,0,0,0,0},       /* mutex */
   {0,0,0,0,0,0,0,0,0,0,0,0,0},/* pcache2 */
   (void*)0,                  /* heap */
   0,                         /* n_heap */
   0, 0,                      /* mn_heap, mx_heap */
   DBSQL_DEFAULT_MMAP_SIZE,  /* sz_mmap */
   DBSQL_MAX_MMAP_SIZE,      /* mx_mmap */
   (void*)0,                  /* page */
   0,                         /* sz_page */
   DBSQL_DEFAULT_PCACHE_INITSZ, /* n_page */
   0,                         /* mx_parser_stack */
   0,                         /* shared_cache_enabled */
   DBSQL_SORTER_PMASZ,       /* sz_pma */
   /* All the rest should always be initialized to zero */
   0,                         /* is_init */
   0,                         /* in_progress */
   0,                         /* is_mutex_init */
   0,                         /* is_malloc_init */
   0,                         /* is_p_cache_init */
   0,                         /* n_ref_init_mutex */
   0,                         /* init_mutex */
   0,                         /* x_log */
   0,                         /* log_arg */
#ifdef DBSQL_ENABLE_SQLLOG
   0,                         /* sqllog */
   0,                         /* sqllog_arg */
#endif
#ifdef DBSQL_VDBE_COVERAGE
   0,                         /* vdbe_branch */
   0,                         /* vbe_branch_arg */
#endif
#ifndef DBSQL_OMIT_DESERIALIZE
   DBSQL_MEMDB_DEFAULT_MAXSIZE,   /* mx_memdb_size */
#endif
#ifndef DBSQL_UNTESTABLE
   0,                         /* test_callback */
#endif
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
   0,                         /* m_no_visible_rowid.  0 == allow rowid-in-view */
#endif
   0,                         /* localtime_fault */
   0,                         /* alt_localtime */
   0x7ffffffe,                /* i_once_reset_threshold */
   DBSQL_DEFAULT_SORTERREF_SIZE,   /* sz_sorter_ref */
   0,                         /* i_prng_seed */
#ifdef DBSQL_DEBUG
   {0,0,0,0,0,0},             /* tunes */
#endif
};

/*
** hash_t table for global functions - functions common to all
** database connections.  After initialization, this table is
** read-only.
*/
func_def_hash_t __builtin_functions;

#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_DEBUG)
/*
** Counter used for coverage testing.  Does not come into play for
** release builds.
**
** Access to this global variable is not mutex protected.  This might
** result in TSAN warnings.  But as the variable does not exist in
** release builds, that should not be a concern.
*/
unsigned int __coverage_counter;
#endif /* DBSQL_COVERAGE_TEST || DBSQL_DEBUG */

#ifdef VDBE_PROFILE
/*
** The following performance counter can be used in place of
** __hwtime() for profiling.  This is a no-op on standard builds.
*/
u_int64_t __n_profile_cnt = 0;
#endif

/*
** The value of the "pending" byte must be 0x40000000 (1 byte past the
** 1-gibabyte boundary) in a compatible database.  dbsql never uses
** the database page that contains the pending byte.  It never attempts
** to read or write that page.  The pending byte page is set aside
** for use by the VFS layers as space for managing file locks.
**
** During testing, it is often desirable to move the pending byte to
** a different position in the file.  This allows code that has to
** deal with the pending byte to run on files that are much smaller
** than 1 __gi_b.  The dbsql_test_control() interface can be used to
** move the pending byte.
**
** IMPORTANT:  Changing the pending byte to any value other than
** 0x40000000 results in an incompatible database file format!
** Changing the pending byte during operation will result in undefined
** and incorrect behavior.
*/
#ifndef DBSQL_OMIT_WSD
int __pending_byte = 0x40000000;
#endif

/*
** Tracing flags set by DBSQL_TESTCTRL_TRACEFLAGS.
*/
u_int32_t __tree_trace = 0;
u_int32_t __where_trace = 0;

#include "opcodes.h"
/*
** Properties of opcodes.  The OPFLG_INITIALIZER macro is
** created by mkopcodeh.awk during compilation.  Data is obtained
** from the comments following the "case OP_xxxx:" statements in
** the vdbe.c file.  
*/
const unsigned char __opcode_property[] = OPFLG_INITIALIZER;

/*
** Name of the default collating sequence
*/
const char __str_binary[] = "BINARY";

/*
** Standard typenames.  These names must match the COLTYPE_* definitions.
** Adjust the DBSQL_N_STDTYPE value if adding or removing entries.
**
**    __std_type[]            The actual names of the datatypes.
**
**    __std_type_len[]         The length (in bytes) of each entry
**                                in __std_type[].
**
**    __std_type_affinity[]    The affinity associated with each entry
**                                in __std_type[].
*/
const unsigned char __std_type_len[] = { 3, 4, 3, 7, 4, 4 };
const char __std_type_affinity[] = {
  DBSQL_AFF_NUMERIC,
  DBSQL_AFF_BLOB,
  DBSQL_AFF_INTEGER,
  DBSQL_AFF_INTEGER,
  DBSQL_AFF_REAL,
  DBSQL_AFF_TEXT
};
const char *__std_type[] = {
  "ANY",
  "BLOB",
  "INT",
  "INTEGER",
  "REAL",
  "TEXT"
};
