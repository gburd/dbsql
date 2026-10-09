/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
*/
#ifndef _FTS5INT_H
#define _FTS5INT_H

#include "fts5.h"
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1

#include <string.h>
#include <assert.h>
#include <stddef.h>

#ifndef DBSQL_AMALGAMATION

typedef unsigned char  u_int8_t;
typedef unsigned int   u_int32_t;
typedef unsigned short u_int16_t;
typedef short int16_t;
typedef int64_t int64_t;
typedef u_int64_t u_int64_t;

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(x) ((int)(sizeof(x) / sizeof(x[0])))
#endif

#define testcase(x)

#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_MUTATION_TEST)
# define DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS 1
#endif
#if defined(DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif

#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))

/*
** Constants for the largest and smallest possible 64-bit signed integers.
*/
# define LARGEST_INT64  (0xffffffff|(((int64_t)0x7fffffff)<<32))
# define SMALLEST_INT64 (((int64_t)-1) - LARGEST_INT64)

/*
** This macro is used in a single assert() within fts5 to check that an
** allocation is aligned to an 8-byte boundary. But it is a complicated
** macro to get right for multiple platforms without generating warnings.
** So instead of reproducing the entire definition from dbsql_int.h, we
** just do without this assert() for the rare non-amalgamation builds.
*/
#define EIGHT_BYTE_ALIGNMENT(x) 1

/*
** Macros needed to provide flexible arrays in a portable way
*/
#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

#endif /* DBSQL_AMALGAMATION */

/*
** Constants for the largest and smallest possible 32-bit signed integers.
*/
# define LARGEST_INT32  ((int)(0x7fffffff))
# define SMALLEST_INT32 ((int)((-1) - LARGEST_INT32))

/* Truncate very long tokens to this many bytes. Hard limit is 
** (65536-1-1-4-9)==65521 bytes. The limiting factor is the 16-bit offset
** field that occurs at the start of each leaf page (see fts5_index.c). */
#define FTS5_MAX_TOKEN_SIZE 32768

/*
** Maximum number of prefix indexes on single FTS5 table. This must be
** less than 32. If it is set to anything large than that, an #error
** directive in fts5_index.c will cause the build to fail.
*/
#define FTS5_MAX_PREFIX_INDEXES 31

/*
** Maximum segments permitted in a single index 
*/
#define FTS5_MAX_SEGMENT 2000

#define FTS5_DEFAULT_NEARDIST 10
#define FTS5_DEFAULT_RANK     "bm25"

/* Name of rank and rowid columns */
#define FTS5_RANK_NAME "rank"
#define FTS5_ROWID_NAME "rowid"

#ifdef DBSQL_DEBUG
# define FTS5_CORRUPT __fts5_corrupt()
int __fts5_corrupt(void);
#else
# define FTS5_CORRUPT DBSQL_CORRUPT_VTAB
#endif

/*
** The assert_nc() macro is similar to the assert() macro, except that it
** is used for assert() conditions that are true only if it can be 
** guranteed that the database is not corrupt.
*/
#ifdef DBSQL_DEBUG
extern int dbsql_fts5_may_be_corrupt;
# define assert_nc(x) assert(dbsql_fts5_may_be_corrupt || (x))
#else
# define assert_nc(x) assert(x)
#endif

/*
** A version of memcmp() that does not cause asan errors if one of the pointer
** parameters is NULL and the number of bytes to compare is zero.
*/
#define FTS5_MEMCMP(s1, s2, n) ((n)<=0 ? 0 : memcmp((s1), (s2), (n)))

/* Mark a function parameter as unused, to suppress nuisance compiler
** warnings. */
#ifndef UNUSED_PARAM
# define UNUSED_PARAM(X)  (void)(X)
#endif

#ifndef UNUSED_PARAM2
# define UNUSED_PARAM2(X, Y)  (void)(X), (void)(Y)
#endif

typedef struct fts5_global_t fts5_global_t;
typedef struct fts5_colset_t fts5_colset_t;

/* If a NEAR() clump or phrase may only match a specific set of columns, 
** then an object of the following type is used to record the set of columns.
** Each entry in the cols[] array is a column that may be matched.
**
** This object is used by fts5_expr.c and fts5_index.c.
*/
struct fts5_colset_t {
  int n_col;
  int cols[FLEXARRAY];
};

/* Size (int bytes) of a complete fts5_colset_t object with N columns. */
#define SZ_FTS5COLSET(N) (sizeof(int64_t)*((N+2)/2))

/**************************************************************************
** Interface to code in fts5_config.c. fts5_config.c contains contains code
** to parse the arguments passed to the CREATE VIRTUAL TABLE statement.
*/

typedef struct fts5_config_t fts5_config_t;
typedef struct fts5_tokenizer_config_t fts5_tokenizer_config_t;

struct fts5_tokenizer_config_t {
  fts5_tokenizer_t *p_tok;
  fts5_tokenizer_v2 *api2;
  fts5_tokenizer *api1;
  const char **args;
  int n_arg;
  int pattern;                   /* FTS_PATTERN_XXX constant */
  const char *locale;            /* Current locale to use */
  int n_locale;                    /* Size of locale in bytes */
};

/*
** An instance of the following structure encodes all information that can
** be gleaned from the CREATE VIRTUAL TABLE statement.
**
** And all information loaded from the %_config table.
**
** n_automerge:
**   The minimum number of segments that an auto-merge operation should
**   attempt to merge together. A value of 1 sets the object to use the 
**   compile time default. Zero disables auto-merge altogether.
**
** contentless_delete:
**   True if the contentless_delete option was present in the CREATE 
**   VIRTUAL TABLE statement.
**
** content:
**
** content_rowid:
**   The value of the content_rowid= option, if one was specified. Or 
**   the string "rowid" otherwise. This text is not quoted - if it is
**   used as part of an SQL statement it needs to be quoted appropriately.
**
** content_exprlist:
**
** errmsg:
**   This exists in order to allow the fts5_index.c module to return a 
**   decent error message if it encounters a file-format version it does
**   not understand.
**
** columnsize:
**   True if the %_docsize table is created.
**
** prefix_index:
**   This is only used for debugging. If set to false, any prefix indexes
**   are ignored. This value is configured using:
**
**       INSERT INTO tbl(tbl, rank) VALUES('prefix-index', $prefix_index);
**
** locale:
**   Set to true if locale=1 was specified when the table was created.
*/
struct fts5_config_t {
  DBSQL *db;                    /* Database handle */
  fts5_global_t *__p_global;            /* Global fts5 object for handle db */
  char *z_db;                      /* Database holding FTS index (e.g. "main") */
  char *name;                    /* Name of FTS index */
  int n_col;                       /* Number of columns */
  char **cols;                   /* column_t names */
  u_int8_t *ab_unindexed;                /* True for unindexed columns */
  int n_prefix;                    /* Number of prefix indexes */
  int *__a_prefix;                   /* Sizes in bytes of n_prefix prefix indexes */
  int content;                   /* An FTS5_CONTENT value */
  int contentless_delete;         /* "contentless_delete=" option (dflt==0) */
  int contentless_unindexed;      /* "contentless_unindexed=" option (dflt=0) */
  char *z_content;                 /* content table */ 
  char *content_rowid;            /* "content_rowid=" option value */ 
  int columnsize;                /* "columnsize=" option value (dflt==1) */
  int tokendata;                 /* "tokendata=" option value (dflt==0) */
  int locale;                    /* "locale=" option value (dflt==0) */
  int detail;                    /* FTS5_DETAIL_XXX value */
  char *content_exprlist;
  fts5_tokenizer_config_t t;
  int lock;                      /* True when table is preparing statement */
  

  /* Values loaded from the %_config table */
  int i_version;                   /* fts5 file format 'version' */
  int i_cookie;                    /* Incremented when %_config is modified */
  int pgsz;                       /* Approximate page size used in %_data */
  int n_automerge;                 /* 'automerge' setting */
  int n_crisis_merge;               /* Maximum allowed segments per level */
  int n_usermerge;                 /* 'usermerge' setting */
  int n_hash_size;                  /* Bytes of memory for in-memory hash */
  char *z_rank;                    /* Name of rank function */
  char *z_rank_args;                /* Arguments to rank function */
  int secure_delete;              /* 'secure-delete' */
  int n_delete_merge;               /* 'deletemerge' */
  int prefix_insttoken;           /* 'prefix-insttoken' */

  /* If non-NULL, points to dbsql_vtab_t.base.errmsg. Often NULL. */
  char **errmsg;

#ifdef DBSQL_DEBUG
  int prefix_index;               /* True to use prefix-indexes */
#endif
};

/* Current expected value of %_config table 'version' field. And
** the expected version if the 'secure-delete' option has ever been
** set on the table.  */
#define FTS5_CURRENT_VERSION               4
#define FTS5_CURRENT_VERSION_SECUREDELETE  5

#define FTS5_CONTENT_NORMAL    0
#define FTS5_CONTENT_NONE      1
#define FTS5_CONTENT_EXTERNAL  2
#define FTS5_CONTENT_UNINDEXED 3

#define FTS5_DETAIL_FULL      0
#define FTS5_DETAIL_NONE      1
#define FTS5_DETAIL_COLUMNS   2

#define FTS5_PATTERN_NONE     0
#define FTS5_PATTERN_LIKE     65  /* matches DBSQL_INDEX_CONSTRAINT_LIKE */
#define FTS5_PATTERN_GLOB     66  /* matches DBSQL_INDEX_CONSTRAINT_GLOB */

int __fts5_config_parse(
    fts5_global_t*, DBSQL*, int, const char **, fts5_config_t**, char**
);
void __fts5_config_free(fts5_config_t*);

int __fts5_config_declare_vtab(fts5_config_t *config);

int __fts5_tokenize(
  fts5_config_t *config,            /* FTS5 Configuration object */
  int flags,                      /* FTS5_TOKENIZE_* flags */
  const char *text, int n_text,   /* Text to tokenize */
  void *ctx,                     /* Context passed to token() */
  int (*token)(void*, int, const char*, int, int, int)    /* Callback */
);

void __fts5_dequote(char *z);

/* Load the contents of the %_config table */
int __fts5_config_load(fts5_config_t*, int);

/* Set the value of a single config attribute */
int __fts5_config_set_value(fts5_config_t*, const char*, dbsql_value_t*, int*);

int __fts5_config_parse_rank(const char*, char**, char**);

void __fts5_config_errmsg(fts5_config_t *config, const char *fmt, ...);

/*
** End of interface to code in fts5_config.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_buffer.c.
*/

/*
** Buffer object for the incremental building of string data.
*/
typedef struct fts5_buffer_t fts5_buffer_t;
struct fts5_buffer_t {
  u_int8_t *p;
  int n;
  int n_space;
};

int __fts5_buffer_size(int*, fts5_buffer_t*, u_int32_t);
void __fts5_buffer_append_varint(int*, fts5_buffer_t*, int64_t);
void __fts5_buffer_append_blob(int*, fts5_buffer_t*, u_int32_t, const u_int8_t*);
void __fts5_buffer_append_string(int *, fts5_buffer_t*, const char*);
void __fts5_buffer_free(fts5_buffer_t*);
void __fts5_buffer_zero(fts5_buffer_t*);
void __fts5_buffer_set(int*, fts5_buffer_t*, int, const u_int8_t*);
void __fts5_buffer_append_printf(int *, fts5_buffer_t*, char *fmt, ...);

char *__fts5_mprintf(int *rc, const char *fmt, ...);

#define FTS5_BUFFER_ZERO(x)             __fts5_buffer_zero(x)
#define FTS5_BUFFER_APPEND_VARINT(a,b,c) __fts5_buffer_append_varint(a,b,(int64_t)c)
#define FTS5_BUFFER_FREE(a)             __fts5_buffer_free(a)
#define FTS5_BUFFER_APPEND_BLOB(a,b,c,d) __fts5_buffer_append_blob(a,b,c,d)
#define FTS5_BUFFER_SET(a,b,c,d)        __fts5_buffer_set(a,b,c,d)

#define FTS5_BUFFER_GROW(rc,buf,nn) ( \
  (u_int32_t)((buf)->n) + (u_int32_t)(nn) <= (u_int32_t)((buf)->n_space) ? 0 : \
    __fts5_buffer_size((rc),(buf),(nn)+(buf)->n) \
)

/* Write and decode big-endian 32-bit integer values */
void __fts5_put32(u_int8_t*, int);
int __fts5_get32(const u_int8_t*);

#define FTS5_POS2COLUMN(i_pos) (int)((i_pos >> 32) & 0x7FFFFFFF)
#define FTS5_POS2OFFSET(i_pos) (int)(i_pos & 0x7FFFFFFF)

typedef struct fts5_poslist_reader_t fts5_poslist_reader_t;
struct fts5_poslist_reader_t {
  /* Variables used only by __fts5_poslist_iter_xxx() functions. */
  const u_int8_t *a;                    /* Position list to iterate through */
  int n;                          /* Size of buffer at a[] in bytes */
  int i;                          /* Current offset in a[] */

  u_int8_t flag;                       /* For client use (any custom purpose) */

  /* Output variables */
  u_int8_t eof;                        /* Set to true at EOF */
  int64_t i_pos;                       /* (i_col<<32) + i_pos */
};
int __fts5_poslist_reader_init(
  const u_int8_t *a, int n,             /* Poslist buffer to iterate through */
  fts5_poslist_reader_t *iter        /* Iterator object to initialize */
);
int __fts5_poslist_reader_next(fts5_poslist_reader_t*);

typedef struct fts5_poslist_writer_t fts5_poslist_writer_t;
struct fts5_poslist_writer_t {
  int64_t i_prev;
};
int __fts5_poslist_writer_append(fts5_buffer_t*, fts5_poslist_writer_t*, int64_t);
void __fts5_poslist_safe_append(fts5_buffer_t*, int64_t*, int64_t);

int __fts5_poslist_next64(
  const u_int8_t *a, int n,             /* Buffer containing poslist */
  int *pi,                        /* IN/OUT: Offset within a[] */
  int64_t *pi_off                      /* IN/OUT: Current offset */
);

/* Malloc utility */
void *__fts5_malloc_zero(int *rc, int64_t n_byte);
char *__fts5_strndup(int *rc, const char *in, int n_in);

/* Character set tests (like isspace(), isalpha() etc.) */
int __fts5_is_bareword(char t);


/* Bucket of terms object used by the integrity-check in offsets=0 mode. */
typedef struct fts5_termset_t fts5_termset_t;
int __fts5_termset_new(fts5_termset_t**);
int __fts5_termset_add(fts5_termset_t*, int, const char*, int, int *pb_present);
void __fts5_termset_free(fts5_termset_t*);

/*
** End of interface to code in fts5_buffer.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_index.c. fts5_index.c contains contains code
** to access the data stored in the %_data table.
*/

typedef struct fts5_index_t fts5_index_t;
typedef struct fts5_index_iter_t fts5_index_iter_t;

struct fts5_index_iter_t {
  int64_t i_rowid;
  const u_int8_t *data;
  int n_data;
  u_int8_t eof;
};

#define __fts5_iter_eof(x) ((x)->eof)

/*
** Values used as part of the flags argument passed to __index_query().
*/
#define FTS5INDEX_QUERY_PREFIX      0x0001  /* Prefix query */
#define FTS5INDEX_QUERY_DESC        0x0002  /* Docs in descending rowid order */
#define FTS5INDEX_QUERY_TEST_NOIDX  0x0004  /* Do not use prefix index */
#define FTS5INDEX_QUERY_SCAN        0x0008  /* Scan query (fts5vocab) */

/* The following are used internally by the fts5_index.c module. They are
** defined here only to make it easier to avoid clashes with the flags
** above. */
#define FTS5INDEX_QUERY_SKIPEMPTY   0x0010
#define FTS5INDEX_QUERY_NOOUTPUT    0x0020
#define FTS5INDEX_QUERY_SKIPHASH    0x0040
#define FTS5INDEX_QUERY_NOTOKENDATA 0x0080
#define FTS5INDEX_QUERY_SCANONETERM 0x0100

/*
** Create/destroy an fts5_index_t object.
*/
int __fts5_index_open(fts5_config_t *config, int create, fts5_index_t**, char**);
int __fts5_index_close(fts5_index_t *p);

/*
** Return a simple checksum value based on the arguments.
*/
u_int64_t __fts5_index_entry_cksum(
  int64_t i_rowid, 
  int i_col, 
  int i_pos, 
  int i_idx,
  const char *term,
  int n_term
);

/*
** Argument p points to a buffer containing utf-8 text that is n bytes in 
** size. Return the number of bytes in the n_char character prefix of the
** buffer, or 0 if there are less than n_char characters in total.
*/
int __fts5_index_charlen_to_bytelen(
  const char *p, 
  int n_byte, 
  int n_char
);

/*
** Open a new iterator to iterate though all rowids that match the 
** specified token or token prefix.
*/
int __fts5_index_query(
  fts5_index_t *p,                   /* FTS index to query */
  const char *token, int n_token, /* Token (or prefix) to query for */
  int flags,                      /* Mask of FTS5INDEX_QUERY_X flags */
  fts5_colset_t *colset,            /* Match these columns only */
  fts5_index_iter_t **iter          /* OUT: New iterator object */
);

/*
** The various operations on open token or token prefix iterators opened
** using __fts5_index_query().
*/
int __fts5_iter_next(fts5_index_iter_t*);
int __fts5_iter_next_from(fts5_index_iter_t*, int64_t i_match);

/*
** Close an iterator opened by __fts5_index_query().
*/
void __fts5_iter_close(fts5_index_iter_t*);

/*
** Close the reader blob handle, if it is open.
*/
void __fts5_index_close_reader(fts5_index_t*);

/*
** This interface is used by the fts5vocab module.
*/
const char *__fts5_iter_term(fts5_index_iter_t*, int*);
int __fts5_iter_next_scan(fts5_index_iter_t*);
void *__fts5_structure_ref(fts5_index_t*);
void __fts5_structure_release(void*);
int __fts5_structure_test(fts5_index_t*, void*);

/*
** Used by inst_token():
*/
int __fts5_iter_token(
  fts5_index_iter_t *index_iter, 
  const char *token, int n_token,
  int64_t i_rowid,
  int i_col, 
  int i_off, 
  const char **out, int *pn_out
);

/*
** Insert or remove data to or from the index. Each time a document is 
** added to or removed from the index, this function is called one or more
** times.
**
** For an insert, it must be called once for each token in the new document.
** If the operation is a delete, it must be called (at least) once for each
** unique token in the document with an i_col value less than zero. The i_pos
** argument is ignored for a delete.
*/
int __fts5_index_write(
  fts5_index_t *p,                   /* index_t to write to */
  int i_col,                       /* column_t token appears in (-ve -> delete) */
  int i_pos,                       /* Position of token within column */
  const char *token, int n_token  /* Token to add or remove to or from index */
);

/*
** Indicate that subsequent calls to __fts5_index_write() pertain to
** document i_docid.
*/
int __fts5_index_begin_write(
  fts5_index_t *p,                   /* index_t to write to */
  int b_delete,                    /* True if current operation is a delete */
  int64_t i_docid                      /* Docid to add or remove data from */
);

/*
** Flush any data stored in the in-memory hash tables to the database.
** Also close any open blob handles.
*/
int __fts5_index_sync(fts5_index_t *p);

/*
** Discard any data stored in the in-memory hash tables. Do not write it
** to the database. Additionally, assume that the contents of the %_data
** table may have changed on disk. So any in-memory caches of %_data 
** records must be invalidated.
*/
int __fts5_index_rollback(fts5_index_t *p);

/*
** Get or set the "averages" values.
*/
int __fts5_index_get_averages(fts5_index_t *p, int64_t *pn_row, int64_t *an_size);
int __fts5_index_set_averages(fts5_index_t *p, const u_int8_t*, int);

/*
** Functions called by the storage module as part of integrity-check.
*/
int __fts5_index_integrity_check(fts5_index_t*, u_int64_t cksum, int use_cksum);

/* 
** Called during virtual module initialization to register UDF 
** fts5_decode() with dbsql 
*/
int __fts5_index_init(DBSQL*);

int __fts5_index_set_cookie(fts5_index_t*, int);

/*
** Return the total number of entries read from the %_data table by 
** this connection since it was created.
*/
int __fts5_index_reads(fts5_index_t *p);

int __fts5_index_reinit(fts5_index_t *p);
int __fts5_index_optimize(fts5_index_t *p);
int __fts5_index_merge(fts5_index_t *p, int n_merge);
int __fts5_index_reset(fts5_index_t *p);

int __fts5_index_load_config(fts5_index_t *p);

int __fts5_index_get_origin(fts5_index_t *p, int64_t *pi_origin);
int __fts5_index_contentless_delete(fts5_index_t *p, int64_t i_origin, int64_t i_rowid);

void __fts5_index_iter_clear_tokendata(fts5_index_iter_t*);

/* Used to populate hash tables for inst_token in detail=none/column mode. */
int __fts5_index_iter_write_tokendata(
    fts5_index_iter_t*, const char*, int, int64_t i_rowid, int i_col, int i_off
);

/*
** End of interface to code in fts5_index.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_varint.c. 
*/
int __fts5_get_varint32(const unsigned char *p, u_int32_t *v);
int __fts5_get_varint_len(u_int32_t i_val);
u_int8_t __fts5_get_varint(const unsigned char*, u_int64_t*);
int __fts5_put_varint(unsigned char *p, u_int64_t v);

#define FTS5_GET_VARINT32(a,b) __fts5_get_varint32(a,(u_int32_t*)&(b))
#define __fts5_get_varint    __fts5_get_varint

#define FTS5_FAST_GET_VARINT32(a, i_off, n_val) {      \
  n_val = (a)[i_off++];                             \
  if( n_val & 0x80 ){                              \
    i_off--;                                       \
    i_off += FTS5_GET_VARINT32(&(a)[i_off], n_val);    \
  }                                               \
}


/*
** End of interface to code in fts5_varint.c.
**************************************************************************/


/**************************************************************************
** Interface to code in fts5_main.c. 
*/

/*
** Virtual-table object.
*/
typedef struct fts5_table_t fts5_table_t;
struct fts5_table_t {
  dbsql_vtab_t base;              /* Base class used by dbsql core */
  fts5_config_t *config;            /* Virtual table configuration */
  fts5_index_t *p_index;              /* Full-text index */
};

int __fts5_load_tokenizer(fts5_config_t *config);

fts5_table_t *__fts5_table_from_csrid(fts5_global_t*, int64_t);

int __fts5_flush_to_disk(fts5_table_t*);

void __fts5_clear_locale(fts5_config_t *config);
void __fts5_set_locale(fts5_config_t *config, const char *loc, int n_loc);

int __fts5_is_locale_value(fts5_config_t *config, dbsql_value_t *val);
int __fts5_decode_locale_value(dbsql_value_t *val, 
    const char **text, int *pn_text, const char **loc, int *pn_loc
);

/*
** End of interface to code in fts5.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_hash.c. 
*/
typedef struct fts5_hash_t fts5_hash_t;

/*
** Create a hash table, free a hash table.
*/
int __fts5_hash_new(fts5_config_t*, fts5_hash_t**, int *pn_size);
void __fts5_hash_free(fts5_hash_t*);

int __fts5_hash_write(
  fts5_hash_t*,
  int64_t i_rowid,                     /* Rowid for this entry */
  int i_col,                       /* column_t token appears in (-ve -> delete) */
  int i_pos,                       /* Position of token within column */
  char byte,
  const char *token, int n_token  /* Token to add or remove to or from index */
);

/*
** Empty (but do not delete) a hash table.
*/
void __fts5_hash_clear(fts5_hash_t*);

/*
** Return true if the hash is empty, false otherwise.
*/
int __fts5_hash_is_empty(fts5_hash_t*);

int __fts5_hash_query(
  fts5_hash_t*,                      /* hash_t table to query */
  int n_pre,
  const char *term, int n_term,   /* Query term */
  void **obj,                   /* OUT: Pointer to doclist for term */
  int *pn_doclist                  /* OUT: Size of doclist in bytes */
);

int __fts5_hash_scan_init(
  fts5_hash_t*,                      /* hash_t table to query */
  const char *term, int n_term    /* Query prefix */
);
void __fts5_hash_scan_next(fts5_hash_t*);
int __fts5_hash_scan_eof(fts5_hash_t*);
void __fts5_hash_scan_entry(fts5_hash_t *,
  const char **term,            /* OUT: term (nul-terminated) */
  int *pn_term,                    /* OUT: Size of term in bytes */
  const u_int8_t **doclist,           /* OUT: pointer to doclist */
  int *pn_doclist                  /* OUT: size of doclist in bytes */
);



/*
** End of interface to code in fts5_hash.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_storage.c. fts5_storage.c contains contains 
** code to access the data stored in the %_content and %_docsize tables.
*/

#define FTS5_STMT_SCAN_ASC  0     /* SELECT rowid, * FROM ... ORDER BY 1 ASC */
#define FTS5_STMT_SCAN_DESC 1     /* SELECT rowid, * FROM ... ORDER BY 1 DESC */
#define FTS5_STMT_LOOKUP    2     /* SELECT rowid, * FROM ... WHERE rowid=? */

typedef struct fts5_storage_t fts5_storage_t;

int __fts5_storage_open(fts5_config_t*, fts5_index_t*, int, fts5_storage_t**, char**);
int __fts5_storage_close(fts5_storage_t *p);
int __fts5_storage_rename(fts5_storage_t*, const char *name);

int __fts5_drop_all(fts5_config_t*);
int __fts5_create_table(fts5_config_t*, const char*, const char*, int, char **);

int __fts5_storage_delete(fts5_storage_t *p, int64_t, dbsql_value_t**, int);
int __fts5_storage_content_insert(fts5_storage_t *p, int, dbsql_value_t**, int64_t*);
int __fts5_storage_index_insert(fts5_storage_t *p, dbsql_value_t**, int64_t);

int __fts5_storage_integrity(fts5_storage_t *p, int i_arg);

int __fts5_storage_stmt(fts5_storage_t *p, int stmt, dbsql_stmt_t**, char**);
void __fts5_storage_stmt_release(fts5_storage_t *p, int stmt, dbsql_stmt_t*);

int __fts5_storage_docsize(fts5_storage_t *p, int64_t i_rowid, int *cols);
int __fts5_storage_size(fts5_storage_t *p, int i_col, int64_t *pn_avg);
int __fts5_storage_row_count(fts5_storage_t *p, int64_t *pn_row);

int __fts5_storage_sync(fts5_storage_t *p);
int __fts5_storage_rollback(fts5_storage_t *p);

int __fts5_storage_config_value(
    fts5_storage_t *p, const char*, dbsql_value_t*, int
);

int __fts5_storage_delete_all(fts5_storage_t *p);
int __fts5_storage_rebuild(fts5_storage_t *p);
int __fts5_storage_optimize(fts5_storage_t *p);
int __fts5_storage_merge(fts5_storage_t *p, int n_merge);
int __fts5_storage_reset(fts5_storage_t *p);

void __fts5_storage_release_delete_row(fts5_storage_t*);
int __fts5_storage_find_delete_row(fts5_storage_t *p, int64_t i_del);

/*
** End of interface to code in fts5_storage.c.
**************************************************************************/


/**************************************************************************
** Interface to code in fts5_expr.c. 
*/
typedef struct fts5_expr_t fts5_expr_t;
typedef struct fts5_expr_node_t fts5_expr_node_t;
typedef struct fts5_parse_t fts5_parse_t;
typedef struct fts5_token_t fts5_token_t;
typedef struct fts5_expr_phrase_t fts5_expr_phrase_t;
typedef struct fts5_expr_nearset_t fts5_expr_nearset_t;

struct fts5_token_t {
  const char *p;                  /* Token text (not NULL terminated) */
  int n;                          /* Size of buffer p in bytes */
};

/* parser_t a MATCH expression. */
int __fts5_expr_new(
  fts5_config_t *config, 
  int phrase_to_and,
  int i_col,                       /* column_t on LHS of MATCH operator */
  const char *expr,
  fts5_expr_t **pp_new, 
  char **err_p
);
int __fts5_expr_pattern(
  fts5_config_t *config, 
  int glob, 
  int i_col, 
  const char *text, 
  fts5_expr_t **pp
);

/*
** for(rc = __fts5_expr_first(expr, idx, desc);
**     rc==DBSQL_OK && 0==__fts5_expr_eof(expr);
**     rc = __fts5_expr_next(expr)
** ){
**   // The document with rowid i_rowid matches the expression!
**   i64 i_rowid = __fts5_expr_rowid(expr);
** }
*/
int __fts5_expr_first(fts5_expr_t*, fts5_index_t *idx, int64_t i_min, int64_t, int desc);
int __fts5_expr_next(fts5_expr_t*, int64_t i_max);
int __fts5_expr_eof(fts5_expr_t*);
int64_t __fts5_expr_rowid(fts5_expr_t*);

void __fts5_expr_free(fts5_expr_t*);
int __fts5_expr_and(fts5_expr_t **pp1, fts5_expr_t *p2);

/* Called during startup to register a UDF with dbsql */
int __fts5_expr_init(fts5_global_t*, DBSQL*);

int __fts5_expr_phrase_count(fts5_expr_t*);
int __fts5_expr_phrase_size(fts5_expr_t*, int i_phrase);
int __fts5_expr_poslist(fts5_expr_t*, int, const u_int8_t **);

typedef struct fts5_poslist_populator_t fts5_poslist_populator_t;
fts5_poslist_populator_t *__fts5_expr_clear_poslists(fts5_expr_t*, int);
int __fts5_expr_populate_poslists(
    fts5_config_t*, fts5_expr_t*, fts5_poslist_populator_t*, int, const char*, int
);
void __fts5_expr_check_poslists(fts5_expr_t*, int64_t);

int __fts5_expr_clone_phrase(fts5_expr_t*, int, fts5_expr_t**);

int __fts5_expr_phrase_collist(fts5_expr_t *, int, const u_int8_t **, int *);

int __fts5_expr_query_token(fts5_expr_t*, int, int, const char**, int*);
int __fts5_expr_inst_token(fts5_expr_t*, int64_t, int, int, int, int, const char**, int*);
void __fts5_expr_clear_tokens(fts5_expr_t*);

/*******************************************
** The fts5_expr.c API above this point is used by the other hand-written
** C code in this module. The interfaces below this point are called by
** the parser code in fts5parse.y.  */

void __fts5_parse_error(fts5_parse_t *parser, const char *fmt, ...);

fts5_expr_node_t *__fts5_parse_node(
  fts5_parse_t *parser,
  int type,
  fts5_expr_node_t *left,
  fts5_expr_node_t *right,
  fts5_expr_nearset_t *near
);

fts5_expr_node_t *__fts5_parse_implicit_and(
  fts5_parse_t *parser,
  fts5_expr_node_t *left,
  fts5_expr_node_t *right
);

fts5_expr_phrase_t *__fts5_parse_term(
  fts5_parse_t *parser, 
  fts5_expr_phrase_t *phrase, 
  fts5_token_t *token,
  int prefix
);

void __fts5_parse_set_caret(fts5_expr_phrase_t*);

fts5_expr_nearset_t *__fts5_parse_nearset(
  fts5_parse_t*, 
  fts5_expr_nearset_t*,
  fts5_expr_phrase_t* 
);

fts5_colset_t *__fts5_parse_colset(
  fts5_parse_t*, 
  fts5_colset_t*, 
  fts5_token_t *
);

void __fts5_parse_phrase_free(fts5_expr_phrase_t*);
void __fts5_parse_nearset_free(fts5_expr_nearset_t*);
void __fts5_parse_node_free(fts5_expr_node_t*);

void __fts5_parse_set_distance(fts5_parse_t*, fts5_expr_nearset_t*, fts5_token_t*);
void __fts5_parse_set_colset(fts5_parse_t*, fts5_expr_node_t*, fts5_colset_t*);
fts5_colset_t *__fts5_parse_colset_invert(fts5_parse_t*, fts5_colset_t*);
void __fts5_parse_finished(fts5_parse_t *parser, fts5_expr_node_t *p);
void __fts5_parse_near(fts5_parse_t *parser, fts5_token_t*);

/*
** End of interface to code in fts5_expr.c.
**************************************************************************/



/**************************************************************************
** Interface to code in fts5_aux.c. 
*/

int __fts5_aux_init(fts5_api*);
/*
** End of interface to code in fts5_aux.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_tokenizer.c. 
*/

int __fts5_tokenizer_init(fts5_api*);
int __fts5_tokenizer_pattern(
    int (*create)(void*, const char**, int, fts5_tokenizer_t**),
    fts5_tokenizer_t *tok
);
int __fts5_tokenizer_preload(fts5_tokenizer_config_t*);
/*
** End of interface to code in fts5_tokenizer.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_vocab.c. 
*/

int __fts5_vocab_init(fts5_global_t*, DBSQL*);

/*
** End of interface to code in fts5_vocab.c.
**************************************************************************/


/**************************************************************************
** Interface to automatically generated code in fts5_unicode2.c. 
*/
int __fts5_unicode_isdiacritic(int c);
int __fts5_unicode_fold(int c, int remove_diacritic);

int __fts5_unicode_cat_parse(const char*, u_int8_t*);
int __fts5_unicode_category(u_int32_t i_code);
void __fts5_unicode_ascii(u_int8_t*, u_int8_t*);
/*
** End of interface to code in fts5_unicode2.c.
**************************************************************************/

#endif
