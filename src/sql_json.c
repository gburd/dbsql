/*
** 2015-08-12
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
** dbsql JSON functions.
**
** This file began as an extension in ext/misc/json1.c in 2015.  That
** extension proved so useful that it has now been moved into the core.
**
** The original design stored all JSON as pure text, canonical RFC-8259.
** Support for JSON-5 extensions was added with version 3.42.0 (2023-05-16).
** All generated JSON text still conforms strictly to RFC-8259, but text
** with JSON-5 extensions is accepted as input.
**
** Beginning with version 3.45.0 (circa 2024-01-01), these routines also
** accept BLOB values that have JSON encoded using a binary representation
** called "JSONB".  The name JSONB comes from __postgre_sql, however the on-disk
** format for dbsql-JSONB is completely different and incompatible with
** __postgre_sql-JSONB.
**
** Decoding and interpreting JSONB is still O(N) where N is the size of
** the input, the same as text JSON.  However, the constant of proportionality
** for JSONB is much smaller due to faster parsing.  The size of each
** element in JSONB is encoded in its header, so there is no need to search
** for delimiters using persnickety syntax rules.  JSONB seems to be about
** 3x faster than text JSON as a result.  JSONB is also tends to be slightly
** smaller than text JSON, by 5% or 10%, but there are corner cases where
** JSONB can be slightly larger.  So you are not far mistaken to say that
** a JSONB blob is the same size as the equivalent RFC-8259 text.
**
**
** THE JSONB ENCODING:
**
** Every JSON element is encoded in JSONB as a header and a payload.
** The header is between 1 and 9 bytes in size.  The payload is zero
** or more bytes.
**
** The lower 4 bits of the first byte of the header determines the
** element type:
**
**    0:   NULL
**    1:   TRUE
**    2:   FALSE
**    3:   INT        -- RFC-8259 integer literal
**    4:   INT5       -- JSON5 integer literal
**    5:   FLOAT      -- RFC-8259 floating point literal
**    6:   FLOAT5     -- JSON5 floating point literal
**    7:   TEXT       -- Text literal acceptable to both SQL and JSON
**    8:   TEXTJ      -- Text containing RFC-8259 escapes
**    9:   TEXT5      -- Text containing JSON5 and/or RFC-8259 escapes
**   10:   TEXTRAW    -- Text containing unescaped syntax characters
**   11:   ARRAY
**   12:   OBJECT
**
** The other three possible values (13-15) are reserved for future
** enhancements.
**
** The upper 4 bits of the first byte determine the size of the header
** and sometimes also the size of the payload.  If X is the first byte
** of the element and if X>>4 is between 0 and 11, then the payload
** will be that many bytes in size and the header is exactly one byte
** in size.  Other four values for X>>4 (12-15) indicate that the header
** is more than one byte in size and that the payload size is determined
** by the remainder of the header, interpreted as a unsigned big-endian
** integer.
**
**   Value of X>>4         Size integer        Total header size
**   -------------     --------------------    -----------------
**        12           1 byte (0-255)                2
**        13           2 byte (0-65535)              3
**        14           4 byte (0-4294967295)         5
**        15           8 byte (0-1.8e19)             9
**
** The payload size need not be expressed in its minimal form.  For example,
** if the payload size is 10, the size can be expressed in any of 5 different
** ways: (1) (X>>4)==10, (2) (X>>4)==12 following by one 0x0a byte,
** (3) (X>>4)==13 followed by 0x00 and 0x0a, (4) (X>>4)==14 followed by
** 0x00 0x00 0x00 0x0a, or (5) (X>>4)==15 followed by 7 bytes of 0x00 and
** a single byte of 0x0a.  The shorter forms are preferred, of course, but
** sometimes when generating JSONB, the payload size is not known in advance
** and it is convenient to reserve sufficient header space to cover the
** largest possible payload size and then come back later and patch up
** the size when it becomes known, resulting in a non-minimal encoding.
**
** The value (X>>4)==15 is not actually used in the current implementation
** (as dbsql is currently unable to handle BLOBs larger than about 2GB)
** but is included in the design to allow for future enhancements.
**
** The payload follows the header.  NULL, TRUE, and FALSE have no payload and
** their payload size must always be zero.  The payload for INT, INT5,
** FLOAT, FLOAT5, TEXT, TEXTJ, TEXT5, and TEXTROW is text.  Note that the
** "..." or '...' delimiters are omitted from the various text encodings.
** The payload for ARRAY and OBJECT is a list of additional elements that
** are the content for the array or object.  The payload for an OBJECT
** must be an even number of elements.  The first element of each pair is
** the label and must be of type TEXT, TEXTJ, TEXT5, or TEXTRAW.
**
** A valid JSONB blob consists of a single element, as described above.
** Usually this will be an ARRAY or OBJECT element which has many more
** elements as its content.  But the overall blob is just a single element.
**
** Input validation for JSONB blobs simply checks that the element type
** code is between 0 and 12 and that the total size of the element
** (header plus payload) is the same as the size of the BLOB.  If those
** checks are true, the BLOB is assumed to be JSONB and processing continues.
** Errors are only raised if some other miscoding is discovered during
** processing.
**
** Additional information can be found in the doc/jsonb.md file of the
** canonical dbsql source tree.
*/
#ifndef DBSQL_OMIT_JSON
#include "dbsql_int.h"

/* JSONB element types
*/
#define JSONB_NULL     0   /* "null" */
#define JSONB_TRUE     1   /* "true" */
#define JSONB_FALSE    2   /* "false" */
#define JSONB_INT      3   /* integer acceptable to JSON and SQL */
#define JSONB_INT5     4   /* integer in 0x000 notation */
#define JSONB_FLOAT    5   /* float acceptable to JSON and SQL */
#define JSONB_FLOAT5   6   /* float with JSON5 extensions */
#define JSONB_TEXT     7   /* Text compatible with both JSON and SQL */
#define JSONB_TEXTJ    8   /* Text with JSON escapes */
#define JSONB_TEXT5    9   /* Text with JSON-5 escape */
#define JSONB_TEXTRAW 10   /* SQL text that needs escaping for JSON */
#define JSONB_ARRAY   11   /* An array */
#define JSONB_OBJECT  12   /* An object */

/* Human-readable names for the JSONB values.  The index for each
** string must correspond to the JSONB_* integer above.
*/
static const char * const __jsonb_type[] = {
  "null", "true", "false", "integer", "integer", 
  "real", "real", "text",  "text",    "text",
  "text", "array", "object", "", "", "", ""
};

/*
** Growing our own isspace() routine this way is twice as fast as
** the library isspace() function, resulting in a 7% overall performance
** increase for the text-JSON parser.  (Ubuntu14.10 gcc 4.8.4 x64 with -Os).
*/
static const char __json_is_space[] = {
#ifdef DBSQL_ASCII
/*0  1  2  3  4  5  6  7   8  9  a  b  c  d  e  f  */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 1, 1, 0, 0, 1, 0, 0,  /* 0 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 1 */
  1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 2 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 3 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 4 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 5 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 6 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 7 */

  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 8 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 9 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* a */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* b */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* c */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* d */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* e */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* f */
#endif
#ifdef DBSQL_EBCDIC
/*0  1  2  3  4  5  6  7   8  9  a  b  c  d  e  f  */
  0, 0, 0, 0, 0, 1, 0, 0,  0, 0, 0, 0, 0, 1, 0, 0,  /* 0 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 1 */
  0, 0, 0, 0, 0, 1, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 2 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 3 */
  1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 4 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 5 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 6 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 7 */

  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 8 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 9 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* a */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* b */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* c */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* d */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* e */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* f */
#endif

};
#define JSON_ISSPACE(x) (__json_is_space[(unsigned char)x])

/*
** The set of all space characters recognized by JSON_ISSPACE().
** Useful as the second argument to strspn().
*/
#ifdef DBSQL_ASCII
static const char __json_spaces[] = "\011\012\015\040";
#endif
#ifdef DBSQL_EBCDIC
static const char __json_spaces[] = "\005\045\015\100";
#endif


/*
** Characters that are special to JSON.  Control characters,
** '"' and '\\' and '\''.  Actually, '\'' is not special to
** canonical JSON, but it is special in JSON-5, so we include
** it in the set of special characters.
*/
static const char __json_is_ok[256] = {
#ifdef DBSQL_ASCII
/*0  1  2  3  4  5  6  7   8  9  a  b  c  d  e  f  */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 0 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 1 */
  1, 1, 0, 1, 1, 1, 1, 0,  1, 1, 1, 1, 1, 1, 1, 1,  /* 2 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 3 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 4 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 0, 1, 1, 1,  /* 5 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 6 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 7 */

  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 8 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 9 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* a */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* b */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* c */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* d */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* e */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1   /* f */
#endif
#ifdef DBSQL_EBCDIC
/*0  1  2  3  4  5  6  7   8  9  a  b  c  d  e  f  */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 0 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 1 */
  0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,  /* 2 */
  1, 1, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 1, 0,  /* 3 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 4 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 5 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 6 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 0, 1, 0,  /* 7 */

  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 8 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* 9 */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* a */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* b */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* c */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* d */
  0, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1,  /* e */
  1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1   /* f */
#endif
};

/* Objects */
typedef struct json_cache_t json_cache_t;
typedef struct json_string_t json_string_t;
typedef struct json_parse_t json_parse_t;

/*
** Magic number used for the JSON parse cache in dbsql_get_auxdata()
*/
#define JSON_CACHE_ID    (-429938)  /* Cache entry */
#define JSON_CACHE_SIZE  4          /* Max number of cache entries */

/*
** __json_unescape_one_char() returns this invalid code point if it encounters
** a syntax error.
*/
#define JSON_INVALID_CHAR 0x99999

/* A cache mapping JSON text into JSONB blobs.
**
** Each cache entry is a json_parse_t object with the following restrictions:
**
**    *   The read_only flag must be set
**
**    *   The blobs[] array must be owned by the json_parse_t object.  In other
**        words, n_blob_alloc must be non-zero.
**
**    *   edit and delta must be zero.
**
**    *   json must be an rc_str_t.  In other words json_is_rc_str must be true.
*/
struct json_cache_t {
  DBSQL *db;                    /* Database connection */
  int n_used;                      /* Number of active entries in the cache */
  json_parse_t *a[JSON_CACHE_SIZE];  /* One line for each cache entry */
};

/* An instance of this object represents a JSON string
** under construction.  Really, this is a generic string accumulator
** that can be and is used to create strings other than JSON.
**
** If the generated string is longer than will fit into the space[] buffer,
** then it will be an rc_str_t string.  This aids with caching of large
** JSON strings.
*/
struct json_string_t {
  dbsql_func_t *ctx;   /* Function context - put error messages here */
  char *buf;              /* Append JSON content here */
  u_int64_t n_alloc;              /* Bytes of storage available in buf[] */
  u_int64_t n_used;               /* Bytes of buf[] currently used */
  u_int8_t b_static;              /* True if buf is static space */
  u_int8_t err;                 /* True if an error has been encountered */
  char space[100];        /* Initial static space */
};

/* Allowed values for json_string_t.err */
#define JSTRING_OOM         0x01   /* Out of memory */
#define JSTRING_MALFORMED   0x02   /* Malformed JSONB */
#define JSTRING_TOODEEP     0x04   /* JSON nested too deep */
#define JSTRING_ERR         0x08   /* Error already sent to dbsql_result */

/* The "subtype" set for text JSON values passed through using
** dbsql_result_subtype() and dbsql_value_subtype().
*/
#define JSON_SUBTYPE  74    /* Ascii for "J" */

/*
** Bit values for the flags passed into various SQL function implementations
** via the dbsql_user_data() value.
*/
#define JSON_JSON      0x01        /* Result is always JSON */
#define JSON_SQL       0x02        /* Result is always SQL */
#define JSON_ABPATH    0x03        /* Allow abbreviated JSON path specs */
#define JSON_ISSET     0x04        /* json_set(), not json_insert() */
#define JSON_AINS      0x08        /* json_array_insert(), not json_insert() */
#define JSON_BLOB      0x10        /* Use the BLOB output format */

#define JSON_INSERT_TYPE(X)  (((X)&0xC)>>2)


/* A parsed JSON value.  Lifecycle:
**
**   1.  JSON comes in and is parsed into a JSONB value in blobs.  The
**       original text is stored in json.  This step is skipped if the
**       input is JSONB instead of text JSON.
**
**   2.  The blobs[] array is searched using the JSON path notation, if needed.
**       
**   3.  Zero or more changes are made to blobs[] (via json_remove() or
**       json_replace() or json_patch() or similar).
**
**   4.  New JSON text is generated from the blobs[] for output.  This step
**       is skipped if the function is one of the jsonb_* functions that
**       returns JSONB instead of text JSON.
*/
struct json_parse_t {
  u_int8_t *blobs;         /* JSONB representation of JSON value */
  u_int32_t n_blob;         /* Bytes of blobs[] actually used */
  u_int32_t n_blob_alloc;    /* Bytes allocated to blobs[].  0 if blobs is external */
  char *json;       /* Json text used for parsing */
  DBSQL *db;       /* The database connection to which this object belongs */
  int n_json;         /* Length of the json string in bytes */
  u_int32_t n_jp_ref;        /* Number of references to this object */
  u_int32_t i_err;          /* Error location in json[] */
  u_int16_t i_depth;        /* Nesting depth */
  u_int8_t n_err;           /* Number of errors seen */
  u_int8_t oom;            /* Set to true if out of memory */
  u_int8_t json_is_rc_str;   /* True if json is an rc_str_t */
  u_int8_t has_nonstd;      /* True if input uses non-standard features like JSON5 */
  u_int8_t read_only;      /* Do not modify. */
  /* Search and edit information.  See __json_lookup_step() */
  u_int8_t edit;          /* Edit operation to apply */
  int delta;         /* Size change due to the edit */
  u_int32_t n_ins;          /* Number of bytes to insert */
  u_int32_t i_label;        /* Location of label if search landed on an object value */
  u_int8_t *inses;          /* Content to be inserted */
};

/* Allowed values for json_parse_t.edit */
#define JEDIT_DEL   1   /* Delete if exists */
#define JEDIT_REPL  2   /* Overwrite if exists */
#define JEDIT_INS   3   /* Insert if not exists */
#define JEDIT_SET   4   /* Insert or overwrite */
#define JEDIT_AINS  5   /* array_insert() */

/*
** Maximum nesting depth of JSON for this implementation.
**
** This limit is needed to avoid a stack overflow in the recursive
** descent parser.  A depth of 1000 is far deeper than any sane JSON
** should go.  Historical note: This limit was 2000 prior to version 3.42.0
*/
#ifndef DBSQL_JSON_MAX_DEPTH
# define JSON_MAX_DEPTH  1000
#else
# define JSON_MAX_DEPTH DBSQL_JSON_MAX_DEPTH
#endif

/*
** Allowed values for the flgs argument to __json_parse_func_arg();
*/
#define JSON_EDITABLE  0x01   /* Generate a writable json_parse_t object */
#define JSON_KEEPERROR 0x02   /* Return non-NULL even if there is an error */

/**************************************************************************
** Forward references
**************************************************************************/
static void __json_return_string_as_blob(json_string_t*);
static int __json_arg_is_jsonb(dbsql_value_t *json, json_parse_t *p);
static u_int32_t __json_translate_blob_to_text(json_parse_t*,u_int32_t,json_string_t*);
static void __json_return_parse(dbsql_func_t*,json_parse_t*);
static json_parse_t *__json_parse_func_arg(dbsql_func_t*,dbsql_value_t*,u_int32_t);
static void __json_parse_free(json_parse_t*);
static u_int32_t __jsonb_payload_size(const json_parse_t*, u_int32_t, u_int32_t*);
static u_int32_t __json_unescape_one_char(const char*, u_int32_t, u_int32_t*);

/**************************************************************************
** Utility routines for dealing with json_cache_t objects
**************************************************************************/

/*
** Free a json_cache_t object.
*/
static void __json_cache_delete(json_cache_t *p){
  int i;
  for(i=0; i<p->n_used; i++){
    __json_parse_free(p->a[i]);
  }
  __dbsql_db_free(p->db, p);
}
static void __json_cache_delete_generic(void *p){
  __json_cache_delete((json_cache_t*)p);
}

/*
** Insert a new entry into the cache.  If the cache is full, expel
** the least recently used entry.  Return DBSQL_OK on success or a
** result code otherwise.
**
** Cache entries are stored in age order, oldest first.
*/
static int __json_cache_insert(
  dbsql_func_t *ctx,   /* The SQL statement context holding the cache */
  json_parse_t *parser       /* The parse object to be added to the cache */
){
  json_cache_t *p;

  assert( parser->json!=0 );
  assert( parser->json_is_rc_str );
  assert( parser->delta==0 );
  p = dbsql_get_auxdata(ctx, JSON_CACHE_ID);
  if( p==0 ){
    DBSQL *db = dbsql_context_db_handle(ctx);
    p = __db_malloc_zero(db, sizeof(*p));
    if( p==0 ) return DBSQL_NOMEM;
    p->db = db;
    dbsql_set_auxdata(ctx, JSON_CACHE_ID, p, __json_cache_delete_generic);
    p = dbsql_get_auxdata(ctx, JSON_CACHE_ID);
    if( p==0 ) return DBSQL_NOMEM;
  }
  if( p->n_used >= JSON_CACHE_SIZE ){
    __json_parse_free(p->a[0]);
    memmove(p->a, &p->a[1], (JSON_CACHE_SIZE-1)*sizeof(p->a[0]));
    p->n_used = JSON_CACHE_SIZE-1;
  }
  assert( parser->n_blob_alloc>0 );
  parser->edit = 0;
  parser->n_jp_ref++;
  parser->read_only = 1;
  p->a[p->n_used] = parser;
  p->n_used++;
  return DBSQL_OK;
}

/*
** Search for a cached translation the json text supplied by arg.  Return
** the json_parse_t object if found.  Return NULL if not found.
**
** When a match if found, the matching entry is moved to become the
** most-recently used entry if it isn't so already.
**
** The json_parse_t object returned still belongs to the Cache and might
** be deleted at any moment.  If the caller wants the json_parse_t to
** linger, it needs to increment the n_pj_ref reference counter.
*/
static json_parse_t *__json_cache_search(
  dbsql_func_t *ctx,    /* The SQL statement context holding the cache */
  dbsql_value_t *arg      /* Function argument containing SQL text */
){
  json_cache_t *p;
  int i;
  const char *json;
  int n_json;

  if( dbsql_value_type(arg)!=DBSQL_TEXT ){
    return 0;
  }
  json = (const char*)dbsql_value_text(arg);
  if( json==0 ) return 0;
  n_json = dbsql_value_bytes(arg);

  p = dbsql_get_auxdata(ctx, JSON_CACHE_ID);
  if( p==0 ){
    return 0;
  }
  for(i=0; i<p->n_used; i++){
    if( p->a[i]->json==json ) break;
  }
  if( i>=p->n_used ){
    for(i=0; i<p->n_used; i++){
      if( p->a[i]->n_json!=n_json ) continue;
      if( memcmp(p->a[i]->json, json, n_json)==0 ) break;
    }
  }
  if( i<p->n_used ){
    if( i<p->n_used-1 ){
      /* Make the matching entry the most recently used entry */
      json_parse_t *tmp = p->a[i];
      memmove(&p->a[i], &p->a[i+1], (p->n_used-i-1)*sizeof(tmp));
      p->a[p->n_used-1] = tmp;
      i = p->n_used - 1;
    }
    assert( p->a[i]->delta==0 );
    return p->a[i];
  }else{
    return 0;
  }
}

/**************************************************************************
** Utility routines for dealing with json_string_t objects
**************************************************************************/

/* Turn uninitialized bulk memory into a valid json_string_t object
** holding a zero-length string.
*/
static void __json_string_zero(json_string_t *p){
  p->buf = p->space;
  p->n_alloc = sizeof(p->space);
  p->n_used = 0;
  p->b_static = 1;
}

/* Initialize the json_string_t object
*/
static void __json_string_init(json_string_t *p, dbsql_func_t *ctx){
  p->ctx = ctx;
  p->err = 0;
  __json_string_zero(p);
}

/* Free all allocated memory and reset the json_string_t object back to its
** initial state.
*/
static void __json_string_reset(json_string_t *p){
  if( !p->b_static ) __rc_str_unref(p->buf);
  __json_string_zero(p);
}

/* Report an out-of-memory (OOM) condition
*/
static void __json_string_oom(json_string_t *p){
  p->err |= JSTRING_OOM;
  if( p->ctx ) dbsql_result_error_nomem(p->ctx);
  __json_string_reset(p);
}

/* Report JSON nested too deep
*/
static void __json_string_too_deep(json_string_t *p){
  p->err |= JSTRING_TOODEEP;
  assert( p->ctx!=0 );
  dbsql_result_error(p->ctx, "JSON nested too deep", -1);
  __json_string_reset(p);
}

/* Enlarge json->buf so that it can hold at least N more bytes.
** Return zero on success.  Return non-zero on an OOM error
*/
static int __json_string_grow(json_string_t *p, u_int32_t N){
  u_int64_t n_total = N<p->n_alloc ? p->n_alloc*2 : p->n_alloc+N+10;
  char *z_new;
  if( p->b_static ){
    if( p->err ) return 1;
    z_new = __rc_str_new(n_total);
    if( z_new==0 ){
      __json_string_oom(p);
      return DBSQL_NOMEM;
    }
    memcpy(z_new, p->buf, (size_t)p->n_used);
    p->buf = z_new;
    p->b_static = 0;
  }else{
    p->buf = __rc_str_resize(p->buf, n_total);
    if( p->buf==0 ){
      p->err |= JSTRING_OOM;
      __json_string_zero(p);
      return DBSQL_NOMEM;
    }
  }
  p->n_alloc = n_total;
  return DBSQL_OK;
}

/* Append N bytes from in onto the end of the json_string_t string.
*/
static DBSQL_NOINLINE void __json_string_expand_and_append(
  json_string_t *p,
  const char *in,
  u_int32_t N
){
  assert( N>0 );
  if( __json_string_grow(p,N) ) return;
  memcpy(p->buf+p->n_used, in, N);
  p->n_used += N;
}
static void __json_append_raw(json_string_t *p, const char *in, u_int32_t N){
  if( N==0 ) return;
  if( N+p->n_used >= p->n_alloc ){
    __json_string_expand_and_append(p,in,N);
  }else{
    memcpy(p->buf+p->n_used, in, N);
    p->n_used += N;
  }
}
static void __json_append_raw_nz(json_string_t *p, const char *in, u_int32_t N){
  assert( N>0 );
  if( N+p->n_used >= p->n_alloc ){
    __json_string_expand_and_append(p,in,N);
  }else{
    memcpy(p->buf+p->n_used, in, N);
    p->n_used += N;
  }
}

/* Append formatted text (not to exceed N bytes) to the json_string_t.
*/
static void __json_printf(int N, json_string_t *p, const char *format, ...){
  va_list ap;
  if( (p->n_used + N >= p->n_alloc) && __json_string_grow(p, N) ) return;
  va_start(ap, format);
  dbsql_vsnprintf(N, p->buf+p->n_used, format, ap);
  va_end(ap);
  p->n_used += (int)strlen(p->buf+p->n_used);
}

/* Append a single character
*/
static DBSQL_NOINLINE void __json_append_char_expand(json_string_t *p, char c){
  if( __json_string_grow(p,1) ) return;
  p->buf[p->n_used++] = c;
}
static void __json_append_char(json_string_t *p, char c){
  if( p->n_used>=p->n_alloc ){
    __json_append_char_expand(p,c);
  }else{
    p->buf[p->n_used++] = c;
  }
}

/* Remove a single character from the end of the string
*/
static void __json_string_trim_one_char(json_string_t *p){
  if( p->err==0 ){
    assert( p->n_used>0 );
    p->n_used--;
  }
}


/* Make sure there is a zero terminator on p->buf[]
**
** Return true on success.  Return false if an OOM prevents this
** from happening.
*/
static int __json_string_terminate(json_string_t *p){
  __json_append_char(p, 0);
  __json_string_trim_one_char(p);
  return p->err==0;
}

/* Append a comma separator to the output buffer, if the previous
** character is not '[' or '{'.
*/
static void __json_append_separator(json_string_t *p){
  char c;
  if( p->n_used==0 ) return;
  c = p->buf[p->n_used-1];
  if( c=='[' || c=='{' ) return;
  __json_append_char(p, ',');
}

/* c is a control character.  Append the canonical JSON representation
** of that control character to p.
**
** This routine assumes that the output buffer has already been enlarged
** sufficiently to hold the worst-case encoding plus a nul terminator.
*/
static void __json_append_control_char(json_string_t *p, u_int8_t c){
  static const char specials[] = {
     0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,   0,   0,   0, 0,   0,   0, 0, 0
  };
  assert( sizeof(specials)==32 );
  assert( specials['\b']=='b' );
  assert( specials['\f']=='f' );
  assert( specials['\n']=='n' );
  assert( specials['\r']=='r' );
  assert( specials['\t']=='t' );
  assert( c>=0 && c<sizeof(specials) );
  assert( p->n_used+7 <= p->n_alloc );
  if( specials[c] ){
    p->buf[p->n_used] = '\\';
    p->buf[p->n_used+1] = specials[c];
    p->n_used += 2;
  }else{
    p->buf[p->n_used] = '\\';
    p->buf[p->n_used+1] = 'u';
    p->buf[p->n_used+2] = '0';
    p->buf[p->n_used+3] = '0';
    p->buf[p->n_used+4] = "0123456789abcdef"[c>>4];
    p->buf[p->n_used+5] = "0123456789abcdef"[c&0xf];
    p->n_used += 6;
  }
}

/* Append the N-byte string in in to the end of the json_string_t string
** under construction.  Enclose the string in double-quotes ("...") and
** escape any double-quotes or backslash characters contained within the
** string.
**
** This routine is a high-runner.  There is a measurable performance
** increase associated with unwinding the __json_is_ok[] loop.
*/
static void __json_append_string(json_string_t *p, const char *in, u_int32_t N){
  u_int32_t k;
  u_int8_t c;
  const u_int8_t *z = (const u_int8_t*)in;
  if( z==0 ) return;
  if( (N+p->n_used+2 >= p->n_alloc) && __json_string_grow(p,N+2)!=0 ) return;
  p->buf[p->n_used++] = '"';
  while( 1 /*exit-by-break*/ ){
    k = 0;
    /* The following while() is the 4-way unwound equivalent of
    **
    **     while( k<N && __json_is_ok[z[k]] ){ k++; }
    */
    while( 1 /* Exit by break */ ){
      if( k+3>=N ){
        while( k<N && __json_is_ok[z[k]] ){ k++; }
        break;
      }
      if( !__json_is_ok[z[k]] ){
        break;
      }
      if( !__json_is_ok[z[k+1]] ){
        k += 1;
        break;
      }
      if( !__json_is_ok[z[k+2]] ){
        k += 2;
        break;
      }
      if( !__json_is_ok[z[k+3]] ){
        k += 3;
        break;
      }else{
        k += 4;
      }
    }
    if( k>=N ){
      if( k>0 ){
        memcpy(&p->buf[p->n_used], z, k);
        p->n_used += k;
      }
      break;
    }
    if( k>0 ){
      memcpy(&p->buf[p->n_used], z, k);
      p->n_used += k;
      z += k;
      N -= k;
    }
    c = z[0];
    if( c=='"' || c=='\\' ){
      if( (p->n_used+N+3 > p->n_alloc) && __json_string_grow(p,N+3)!=0 ) return;
      p->buf[p->n_used++] = '\\';
      p->buf[p->n_used++] = c;
    }else if( c=='\'' ){
      p->buf[p->n_used++] = c;
    }else{
      if( (p->n_used+N+7 > p->n_alloc) && __json_string_grow(p,N+7)!=0 ) return;
      __json_append_control_char(p, c);
    }
    z++;
    N--;
  }
  p->buf[p->n_used++] = '"';
  assert( p->n_used<p->n_alloc );
}

/*
** Append an dbsql_value_t (such as a function parameter) to the JSON
** string under construction in p.
*/
static void __json_append_sql_value(
  json_string_t *p,                 /* Append to this JSON string */
  dbsql_value_t *value          /* Value to append */
){
  switch( dbsql_value_type(value) ){
    case DBSQL_NULL: {
      __json_append_raw_nz(p, "null", 4);
      break;
    }
    case DBSQL_FLOAT: {
      __json_printf(100, p, "%!0.17g", dbsql_value_double(value));
      break;
    }
    case DBSQL_INTEGER: {
      const char *z = (const char*)dbsql_value_text(value);
      u_int32_t n = (u_int32_t)dbsql_value_bytes(value);
      __json_append_raw(p, z, n);
      break;
    }
    case DBSQL_TEXT: {
      const char *z = (const char*)dbsql_value_text(value);
      u_int32_t n = (u_int32_t)dbsql_value_bytes(value);
      if( dbsql_value_subtype(value)==JSON_SUBTYPE ){
        __json_append_raw(p, z, n);
      }else{
        __json_append_string(p, z, n);
      }
      break;
    }
    default: {
      json_parse_t px;
      memset(&px, 0, sizeof(px));
      if( __json_arg_is_jsonb(value, &px) ){
        __json_translate_blob_to_text(&px, 0, p);
      }else if( p->err==0 ){
        dbsql_result_error(p->ctx, "JSON cannot hold BLOB values", -1);
        p->err = JSTRING_ERR;
        __json_string_reset(p);
      }
      break;
    }
  }
}

/* Make the text in p (which is probably a generated JSON text string)
** the result of the SQL function.
**
** The json_string_t is reset.
**
** If parser and ctx are both non-NULL, then the SQL string in p is
** loaded into the json field of the parser object as a rc_str_t and the
** parser is added to the cache.
*/
static void __json_return_string(
  json_string_t *p,            /* String to return */
  json_parse_t *parser,        /* JSONB source or NULL */
  dbsql_func_t *ctx      /* Where to cache */
){
  assert( (parser!=0)==(ctx!=0) );
  assert( ctx==0 || ctx==p->ctx );
  __json_string_terminate(p);
  if( p->err==0 ){
    int flags = DBSQL_PTR_TO_INT(dbsql_user_data(p->ctx));
    if( flags & JSON_BLOB ){
      __json_return_string_as_blob(p);
    }else if( p->b_static ){
      dbsql_result_text64(p->ctx, p->buf, p->n_used,
                            DBSQL_TRANSIENT, DBSQL_UTF8);
    }else{
      if( parser && parser->json_is_rc_str==0 && parser->n_blob_alloc>0 ){
        int rc;
        parser->json = __rc_str_ref(p->buf);
        parser->n_json = p->n_used;
        parser->json_is_rc_str = 1;
        rc = __json_cache_insert(ctx, parser);
        if( rc==DBSQL_NOMEM ){
          dbsql_result_error_nomem(ctx);
          __json_string_reset(p);
          return;
        }
      }
      dbsql_result_text64(p->ctx, __rc_str_ref(p->buf), p->n_used,
                            __rc_str_unref,
                            DBSQL_UTF8);
    }
  }else if( p->err & JSTRING_OOM ){
    dbsql_result_error_nomem(p->ctx);
  }else if( p->err & JSTRING_TOODEEP ){
    /* error already in p->ctx */
  }else if( p->err & JSTRING_MALFORMED ){
    dbsql_result_error(p->ctx, "malformed JSON", -1);
  }
  __json_string_reset(p);
}

/**************************************************************************
** Utility routines for dealing with json_parse_t objects
**************************************************************************/

/*
** Reclaim all memory allocated by a json_parse_t object.  But do not
** delete the json_parse_t object itself.
*/
static void __json_parse_reset(json_parse_t *parser){
  assert( parser->n_jp_ref<=1 );
  if( parser->json_is_rc_str ){
    __rc_str_unref(parser->json);
    parser->json = 0;
    parser->n_json = 0;
    parser->json_is_rc_str = 0;
  }
  if( parser->n_blob_alloc ){
    __dbsql_db_free(parser->db, parser->blobs);
    parser->blobs = 0;
    parser->n_blob = 0;
    parser->n_blob_alloc = 0;
  }
}

/*
** Decrement the reference count on the json_parse_t object.  When the
** count reaches zero, free the object.
*/
static void __json_parse_free(json_parse_t *parser){
  if( parser ){
    if( parser->n_jp_ref>1 ){
      parser->n_jp_ref--;
    }else{
      __json_parse_reset(parser);
      __dbsql_db_free(parser->db, parser);
    }
  }
}

/**************************************************************************
** Utility routines for the JSON text parser
**************************************************************************/

/*
** Translate a single byte of Hex into an integer.
** This routine only gives a correct answer if h really is a valid hexadecimal
** character:  0..9a..f_a..F.  But unlike __hex_to_int(), it does not
** assert() if the digit is not hex.
*/
static u_int8_t __json_hex_to_int(int h){
#ifdef DBSQL_ASCII
  h += 9*(1&(h>>6));
#endif
#ifdef DBSQL_EBCDIC
  h += 9*(1&~(h>>4));
#endif
  return (u_int8_t)(h & 0xf);
}

/*
** Convert a 4-byte hex string into an integer
*/
static u_int32_t __json_hex_to_int4(const char *z){
  u_int32_t v;
  v = (__json_hex_to_int(z[0])<<12)
    + (__json_hex_to_int(z[1])<<8)
    + (__json_hex_to_int(z[2])<<4)
    + __json_hex_to_int(z[3]);
  return v;
}

/*
** Return true if z[] begins with 2 (or more) hexadecimal digits
*/
static int __json_is2_hex(const char *z){
  return __isxdigit(z[0]) && __isxdigit(z[1]);
}

/*
** Return true if z[] begins with 4 (or more) hexadecimal digits
*/
static int __json_is4_hex(const char *z){
  return __json_is2_hex(z) && __json_is2_hex(&z[2]);
}

/*
** Return the number of bytes of JSON5 whitespace at the beginning of
** the input string z[].
**
** JSON5 whitespace consists of any of the following characters:
**
**    Unicode  UTF-8         Name
**    U+0009   09            horizontal tab
**    U+000a   0a            line feed
**    U+000b   0b            vertical tab
**    U+000c   0c            form feed
**    U+000d   0d            carriage return
**    U+0020   20            space
**    U+00a0   c2 a0         non-breaking space
**    U+1680   e1 9a 80      ogham space mark
**    U+2000   e2 80 80      en quad
**    U+2001   e2 80 81      em quad
**    U+2002   e2 80 82      en space
**    U+2003   e2 80 83      em space
**    U+2004   e2 80 84      three-per-em space
**    U+2005   e2 80 85      four-per-em space
**    U+2006   e2 80 86      six-per-em space
**    U+2007   e2 80 87      figure space
**    U+2008   e2 80 88      punctuation space
**    U+2009   e2 80 89      thin space
**    U+200a   e2 80 8a      hair space
**    U+2028   e2 80 a8      line separator
**    U+2029   e2 80 a9      paragraph separator
**    U+202f   e2 80 af      narrow no-break space (NNBSP)
**    U+205f   e2 81 9f      medium mathematical space (MMSP)
**    U+3000   e3 80 80      ideographical space
**    U+FEFF   ef bb bf      byte order mark
**
** In addition, comments between '/', '*' and '*', '/' and
** from '/', '/' to end-of-line are also considered to be whitespace.
*/
static int json5Whitespace(const char *in){
  int n = 0;
  const u_int8_t *z = (u_int8_t*)in;
  while( 1 /*exit by "goto whitespace_done"*/ ){
    switch( z[n] ){
      case 0x09:
      case 0x0a:
      case 0x0b:
      case 0x0c:
      case 0x0d:
      case 0x20: {
        n++;
        break;
      }
      case '/': {
        if( z[n+1]=='*' && z[n+2]!=0 ){
          int j;
          for(j=n+3; z[j]!='/' || z[j-1]!='*'; j++){
            if( z[j]==0 ) goto whitespace_done;
          }
          n = j+1;
          break;
        }else if( z[n+1]=='/' ){
          int j;
          char c;
          for(j=n+2; (c = z[j])!=0; j++){
            if( c=='\n' || c=='\r' ) break;
            if( 0xe2==(u_int8_t)c && 0x80==(u_int8_t)z[j+1]
             && (0xa8==(u_int8_t)z[j+2] || 0xa9==(u_int8_t)z[j+2])
            ){
              j += 2;
              break;
            }
          }
          n = j;
          if( z[n] ) n++;
          break;
        }
        goto whitespace_done;
      }
      case 0xc2: {
        if( z[n+1]==0xa0 ){
          n += 2;
          break;
        }
        goto whitespace_done;
      }
      case 0xe1: {
        if( z[n+1]==0x9a && z[n+2]==0x80 ){
          n += 3;
          break;
        }
        goto whitespace_done;
      }
      case 0xe2: {
        if( z[n+1]==0x80 ){
          u_int8_t c = z[n+2];
          if( c<0x80 ) goto whitespace_done;
          if( c<=0x8a || c==0xa8 || c==0xa9 || c==0xaf ){
            n += 3;
            break;
          }
        }else if( z[n+1]==0x81 && z[n+2]==0x9f ){
          n += 3;
          break;
        }
        goto whitespace_done;
      }
      case 0xe3: {
        if( z[n+1]==0x80 && z[n+2]==0x80 ){
          n += 3;
          break;
        }
        goto whitespace_done;
      }
      case 0xef: {
        if( z[n+1]==0xbb && z[n+2]==0xbf ){
          n += 3;
          break;
        }
        goto whitespace_done;
      }
      default: {
        goto whitespace_done;
      }
    }
  }
  whitespace_done:
  return n;
}

/*
** Extra floating-point literals to allow in JSON.
*/
static const struct nan_inf_name_t {
  char c1;
  char c2;
  char n;
  char type;
  char n_repl;
  char *match;
  char *repl;
} __a_nan_inf_name[] = {
  { 'i', 'I', 3, JSONB_FLOAT, 7, "inf", "9.0e999" },
  { 'i', 'I', 8, JSONB_FLOAT, 7, "infinity", "9.0e999" },
  { 'n', 'N', 3, JSONB_NULL, 4, "NaN", "null" },
  { 'q', 'Q', 4, JSONB_NULL, 4, "QNaN", "null" },
  { 's', 'S', 4, JSONB_NULL, 4, "SNaN", "null" },
};


/*
** Report the wrong number of arguments for json_insert(), json_replace()
** or json_set().
*/
static void __json_wrong_num_args(
  dbsql_func_t *ctx,
  const char *func_name
){
  char *msg = dbsql_mprintf("json_%s() needs an odd number of arguments",
                               func_name);
  dbsql_result_error(ctx, msg, -1);
  dbsql_free(msg);
}

/****************************************************************************
** Utility routines for dealing with the binary BLOB representation of JSON
****************************************************************************/

/*
** Expand parser->blobs so that it holds at least N bytes.
**
** Return the number of errors.
*/
static int __json_blob_expand(json_parse_t *parser, u_int32_t N){
  u_int8_t *news;
  u_int64_t t;
  assert( N>parser->n_blob_alloc );
  if( parser->n_blob_alloc==0 ){
    t = 100;
  }else{
    t = parser->n_blob_alloc*2;
  }
  if( t<N ) t = N+100;
  news = __db_realloc(parser->db, parser->blobs, t);
  if( news==0 ){ parser->oom = 1; return 1; }
  assert( t<0x7fffffff );
  parser->blobs = news;
  parser->n_blob_alloc = (u_int32_t)t;
  return 0;
}

/*
** If parser->blobs is not previously editable (because it is taken
** from dbsql_value_blob(), as indicated by the fact that
** parser->n_blob_alloc==0 and parser->n_blob>0) then make it editable
** by making a copy into space obtained from malloc.
**
** Return true on success.  Return false on OOM.
*/
static int __json_blob_make_editable(json_parse_t *parser, u_int32_t n_extra){
  u_int8_t *olds;
  u_int32_t n_size;
  assert( !parser->read_only );
  if( parser->oom ) return 0;
  if( parser->n_blob_alloc>0 ) return 1;
  olds = parser->blobs;
  n_size = parser->n_blob + n_extra;
  parser->blobs = 0;
  if( __json_blob_expand(parser, n_size) ){
    return 0;
  }
  assert( parser->n_blob_alloc >= parser->n_blob + n_extra );
  memcpy(parser->blobs, olds, parser->n_blob);
  return 1;
}

/* Expand parser->blobs and append one bytes.
*/
static DBSQL_NOINLINE void __json_blob_expand_and_append_one_byte(
  json_parse_t *parser,
  u_int8_t c
){
  __json_blob_expand(parser, parser->n_blob+1);
  if( parser->oom==0 ){
    assert( parser->n_blob+1<=parser->n_blob_alloc );
    parser->blobs[parser->n_blob++] = c;
  }
}

/* Append a single character.
*/
static void __json_blob_append_one_byte(json_parse_t *parser, u_int8_t c){
  if( parser->n_blob >= parser->n_blob_alloc ){
    __json_blob_expand_and_append_one_byte(parser, c);
  }else{
    parser->blobs[parser->n_blob++] = c;
  }
}

/* Slow version of __json_blob_append_node() that first resizes the
** parser->blobs structure.
*/
static void __json_blob_append_node(json_parse_t*,u_int8_t,u_int64_t,const void*);
static DBSQL_NOINLINE void __json_blob_expand_and_append_node(
  json_parse_t *parser,
  u_int8_t type,
  u_int64_t sz_payload,
  const void *payloads
){
  if( __json_blob_expand(parser, parser->n_blob+sz_payload+9) ) return;
  __json_blob_append_node(parser, type, sz_payload, payloads);
}


/* Append a node type byte together with the payload size and
** possibly also the payload.
**
** If payloads is not NULL, then it is a pointer to the payload which
** is also appended.  If payloads is NULL, the parser->blobs[] array
** is resized (if necessary) so that it is big enough to hold the
** payload, but the payload is not appended and parser->n_blob is left
** pointing to where the first byte of payload will eventually be.
*/
static void __json_blob_append_node(
  json_parse_t *parser,          /* The json_parse_t object under construction */
  u_int8_t type,                   /* Node type.  One of JSONB_* */
  u_int64_t sz_payload,              /* Number of bytes of payload */
  const void *payloads        /* The payload.  Might be NULL */
){
  u_int8_t *a;
  if( parser->n_blob+sz_payload+9 > parser->n_blob_alloc ){
    __json_blob_expand_and_append_node(parser,type,sz_payload,payloads);
    return;
  }
  assert( parser->blobs!=0 );
  a = &parser->blobs[parser->n_blob];
  if( sz_payload<=11 ){
    a[0] = type | (sz_payload<<4);
    parser->n_blob += 1;
  }else if( sz_payload<=0xff ){
    a[0] = type | 0xc0;
    a[1] = sz_payload & 0xff;
    parser->n_blob += 2;
  }else if( sz_payload<=0xffff ){
    a[0] = type | 0xd0;
    a[1] = (sz_payload >> 8) & 0xff;
    a[2] = sz_payload & 0xff;
    parser->n_blob += 3;
  }else{
    a[0] = type | 0xe0;
    a[1] = (sz_payload >> 24) & 0xff;
    a[2] = (sz_payload >> 16) & 0xff;
    a[3] = (sz_payload >> 8) & 0xff;
    a[4] = sz_payload & 0xff;
    parser->n_blob += 5;
  }
  if( payloads ){
    parser->n_blob += sz_payload;
    memcpy(&parser->blobs[parser->n_blob-sz_payload], payloads, sz_payload);
  }
}

/* Change the payload size for the node at index i to be sz_payload.
*/
static int __json_blob_change_payload_size(
  json_parse_t *parser,
  u_int32_t i,
  u_int32_t sz_payload
){
  u_int8_t *a;
  u_int8_t sz_type;
  u_int8_t n_extra;
  u_int8_t n_needed;
  int delta;
  if( parser->oom ) return 0;
  a = &parser->blobs[i];
  sz_type = a[0]>>4;
  if( sz_type<=11 ){
    n_extra = 0;
  }else if( sz_type==12 ){
    n_extra = 1;
  }else if( sz_type==13 ){
    n_extra = 2;
  }else if( sz_type==14 ){
    n_extra = 4;
  }else{
    n_extra = 8;
  }
  if( sz_payload<=11 ){
    n_needed = 0;
  }else if( sz_payload<=0xff ){
    n_needed = 1;
  }else if( sz_payload<=0xffff ){
    n_needed = 2;
  }else{
    n_needed = 4;
  }
  delta = n_needed - n_extra;
  if( delta ){
    u_int32_t newSize = parser->n_blob + delta;
    if( delta>0 ){
      if( newSize>parser->n_blob_alloc && __json_blob_expand(parser, newSize) ){
        return 0;  /* OOM error.  Error state recorded in parser->oom. */
      }
      a = &parser->blobs[i];
      memmove(&a[1+delta], &a[1], parser->n_blob - (i+1));
    }else{
      memmove(&a[1], &a[1-delta], parser->n_blob - (i+1-delta));
    }
    parser->n_blob = newSize;
  }
  if( n_needed==0 ){
    a[0] = (a[0] & 0x0f) | (sz_payload<<4);
  }else if( n_needed==1 ){
    a[0] = (a[0] & 0x0f) | 0xc0;
    a[1] = sz_payload & 0xff;
  }else if( n_needed==2 ){
    a[0] = (a[0] & 0x0f) | 0xd0;
    a[1] = (sz_payload >> 8) & 0xff;
    a[2] = sz_payload & 0xff;
  }else{
    a[0] = (a[0] & 0x0f) | 0xe0;
    a[1] = (sz_payload >> 24) & 0xff;
    a[2] = (sz_payload >> 16) & 0xff;
    a[3] = (sz_payload >> 8) & 0xff;
    a[4] = sz_payload & 0xff;
  }
  return delta;
}

/*
** If z[0] is 'u' and is followed by exactly 4 hexadecimal character,
** then set *op to JSONB_TEXTJ and return true.  If not, do not make
** any changes to *op and return false.
*/
static int __json_is4_hex_b(const char *z, int *op){
  if( z[0]!='u' ) return 0;
  if( !__json_is4_hex(&z[1]) ) return 0;
  *op = JSONB_TEXTJ;
  return 1;
}

/*
** Check a single element of the JSONB in parser for validity.
**
** The element to be checked starts at offset i and must end at on the
** last byte before i_end.
**
** Return 0 if everything is correct.  Return the 1-based byte offset of the
** error if a problem is detected.  (In other words, if the error is at offset
** 0, return 1).
*/
static u_int32_t __jsonb_validity_check(
  const json_parse_t *parser,    /* Input JSONB.  Only blobs and n_blob are used */
  u_int32_t i,                      /* Start of element as parser->blobs[i] */
  u_int32_t i_end,                   /* One more than the last byte of the element */
  u_int32_t i_depth                  /* Current nesting depth */
){
  u_int32_t n, sz, j, k;
  const u_int8_t *z;
  u_int8_t x;
  if( i_depth>JSON_MAX_DEPTH ) return i+1;
  sz = 0;
  n = __jsonb_payload_size(parser, i, &sz);
  if( NEVER(n==0) ) return i+1;          /* Checked by caller */
  if( NEVER(i+n+sz!=i_end) ) return i+1;  /* Checked by caller */
  z = parser->blobs;
  x = z[i] & 0x0f;
  switch( x ){
    case JSONB_NULL:
    case JSONB_TRUE:
    case JSONB_FALSE: {
      return n+sz==1 ? 0 : i+1;
    }
    case JSONB_INT: {
      if( sz<1 ) return i+1;
      j = i+n;
      if( z[j]=='-' ){
        j++;
        if( sz<2 ) return i+1;
      }
      k = i+n+sz;
      while( j<k ){
        if( __isdigit(z[j]) ){
          j++;
        }else{
          return j+1;
        }
      }
      return 0;
    }
    case JSONB_INT5: {
      if( sz<3 ) return i+1;
      j = i+n;
      if( z[j]=='-' ){
        if( sz<4 ) return i+1;
        j++;
      }
      if( z[j]!='0' ) return i+1;
      if( z[j+1]!='x' && z[j+1]!='X' ) return j+2;
      j += 2;
      k = i+n+sz;
      while( j<k ){
        if( __isxdigit(z[j]) ){
          j++;
        }else{
          return j+1;
        }
      }
      return 0;
    }
    case JSONB_FLOAT:
    case JSONB_FLOAT5: {
      u_int8_t seen = 0;   /* 0: initial.  1: '.' seen  2: 'e' seen */
      if( sz<2 ) return i+1;
      j = i+n;
      k = j+sz;
      if( z[j]=='-' ){
        j++;
        if( sz<3 ) return i+1;
      }
      if( z[j]=='.' ){
        if( x==JSONB_FLOAT ) return j+1;
        if( !__isdigit(z[j+1]) ) return j+1;
        j += 2;
        seen = 1;
      }else if( z[j]=='0' && x==JSONB_FLOAT ){
        if( j+3>k ) return j+1;
        if( z[j+1]!='.' && z[j+1]!='e' && z[j+1]!='E' ) return j+1;
        j++;
      }
      for(; j<k; j++){
        if( __isdigit(z[j]) ) continue;
        if( z[j]=='.' ){
          if( seen>0 ) return j+1;
          if( x==JSONB_FLOAT && (j==k-1 || !__isdigit(z[j+1])) ){
            return j+1;
          }
          seen = 1;
          continue;
        }
        if( z[j]=='e' || z[j]=='E' ){
          if( seen==2 ) return j+1;
          if( j==k-1 ) return j+1;
          if( z[j+1]=='+' || z[j+1]=='-' ){
            j++;
            if( j==k-1 ) return j+1;
          }
          seen = 2;
          continue;
        }
        return j+1;
      }
      if( seen==0 ) return i+1;
      return 0;
    }
    case JSONB_TEXT: {
      j = i+n;
      k = j+sz;
      while( j<k ){
        if( !__json_is_ok[z[j]] && z[j]!='\'' ) return j+1;
        j++;
      }
      return 0;
    }
    case JSONB_TEXTJ:
    case JSONB_TEXT5: {
      j = i+n;
      k = j+sz;
      while( j<k ){
        if( !__json_is_ok[z[j]] && z[j]!='\'' ){
          if( z[j]=='"' ){
            if( x==JSONB_TEXTJ ) return j+1;
          }else if( z[j]<=0x1f ){
            /* Control characters in JSON5 string literals are ok */
            if( x==JSONB_TEXTJ ) return j+1;
          }else if( NEVER(z[j]!='\\') || j+1>=k ){
            return j+1;
          }else if( strchr("\"\\/bfnrt",z[j+1])!=0 ){
            j++;
          }else if( z[j+1]=='u' ){
            if( j+5>=k ) return j+1;
            if( !__json_is4_hex((const char*)&z[j+2]) ) return j+1;
            j++;
          }else if( x!=JSONB_TEXT5 ){
            return j+1;
          }else{
            u_int32_t c = 0;
            u_int32_t sz_c = __json_unescape_one_char((const char*)&z[j], k-j, &c);
            if( c==JSON_INVALID_CHAR ) return j+1;
            j += sz_c - 1;
          }
        }
        j++;
      }
      return 0;
    }
    case JSONB_TEXTRAW: {
      return 0;
    }
    case JSONB_ARRAY: {
      u_int32_t sub;
      j = i+n;
      k = j+sz;
      while( j<k ){
        sz = 0;
        n = __jsonb_payload_size(parser, j, &sz);
        if( n==0 ) return j+1;
        if( j+n+sz>k ) return j+1;
        sub = __jsonb_validity_check(parser, j, j+n+sz, i_depth+1);
        if( sub ) return sub;
        j += n + sz;
      }
      assert( j==k );
      return 0;
    }
    case JSONB_OBJECT: {
      u_int32_t cnt = 0;
      u_int32_t sub;
      j = i+n;
      k = j+sz;
      while( j<k ){
        sz = 0;
        n = __jsonb_payload_size(parser, j, &sz);
        if( n==0 ) return j+1;
        if( j+n+sz>k ) return j+1;
        if( (cnt & 1)==0 ){
          x = z[j] & 0x0f;
          if( x<JSONB_TEXT || x>JSONB_TEXTRAW ) return j+1;
        }
        sub = __jsonb_validity_check(parser, j, j+n+sz, i_depth+1);
        if( sub ) return sub;
        cnt++;
        j += n + sz;
      }
      assert( j==k );
      if( (cnt & 1)!=0 ) return j+1;
      return 0;
    }
    default: {
      return i+1;
    }
  }
}

/*
** Translate a single element of JSON text at parser->json[i] into
** its equivalent binary JSONB representation.  Append the translation into
** parser->blobs[] beginning at parser->n_blob.  The size of
** parser->blobs[] is increased as necessary.
**
** Return the index of the first character past the end of the element parsed,
** or one of the following special result codes:
**
**      0    End of input
**     -1    Syntax error or OOM
**     -2    '}' seen   \
**     -3    ']' seen    \___  For these returns, parser->i_err is set to
**     -4    ',' seen    /     the index in json[] of the seen character
**     -5    ':' seen   /
*/
static int __json_translate_text_to_blob(json_parse_t *parser, u_int32_t i){
  char c;
  u_int32_t j;
  u_int32_t i_this, i_start;
  int x;
  u_int8_t t;
  const char *z = parser->json;
json_parse_restart:
  switch( (u_int8_t)z[i] ){
  case '{': {
    /* parser_t object */
    i_this = parser->n_blob;
    __json_blob_append_node(parser, JSONB_OBJECT, parser->n_json-i, 0);
    if( ++parser->i_depth > JSON_MAX_DEPTH ){
      parser->i_err = i;
      return -1;
    }
    i_start = parser->n_blob;
    for(j=i+1;;j++){
      u_int32_t i_blob = parser->n_blob;
      x = __json_translate_text_to_blob(parser, j);
      if( x<=0 ){
        int op;
        if( x==(-2) ){
          j = parser->i_err;
          if( parser->n_blob!=(u_int32_t)i_start ) parser->has_nonstd = 1;
          break;
        }
        j += json5Whitespace(&z[j]);
        op = JSONB_TEXT;
        if( __json_id1(z[j]) 
         || (z[j]=='\\' && __json_is4_hex_b(&z[j+1], &op))
        ){
          int k = j+1;
          while( (__json_id2(z[k]) && json5Whitespace(&z[k])==0)
            || (z[k]=='\\' && __json_is4_hex_b(&z[k+1], &op))
          ){
            k++;
          }
          assert( i_blob==parser->n_blob );
          __json_blob_append_node(parser, op, k-j, &z[j]);
          parser->has_nonstd = 1;
          x = k;
        }else{
          if( x!=-1 ) parser->i_err = j;
          return -1;
        }
      }
      if( parser->oom ) return -1;
      t = parser->blobs[i_blob] & 0x0f;
      if( t<JSONB_TEXT || t>JSONB_TEXTRAW ){
        parser->i_err = j;
        return -1;
      }
      j = x;
      if( z[j]==':' ){
        j++;
      }else{
        if( JSON_ISSPACE(z[j]) ){
          /* strspn() is not helpful here */
          do{ j++; }while( JSON_ISSPACE(z[j]) );
          if( z[j]==':' ){
            j++;
            goto parse_object_value;
          }
        }
        x = __json_translate_text_to_blob(parser, j);
        if( x!=(-5) ){
          if( x!=(-1) ) parser->i_err = j;
          return -1;
        }
        j = parser->i_err+1;
      }
    parse_object_value:
      x = __json_translate_text_to_blob(parser, j);
      if( x<=0 ){
        if( x!=(-1) ) parser->i_err = j;
        return -1;
      }
      j = x;
      if( z[j]==',' ){
        continue;
      }else if( z[j]=='}' ){
        break;
      }else{
        if( JSON_ISSPACE(z[j]) ){
          j += 1 + (u_int32_t)strspn(&z[j+1], __json_spaces);
          if( z[j]==',' ){
            continue;
          }else if( z[j]=='}' ){
            break;
          }
        }
        x = __json_translate_text_to_blob(parser, j);
        if( x==(-4) ){
          j = parser->i_err;
          continue;
        }
        if( x==(-2) ){
          j = parser->i_err;
          break;
        }
      }
      parser->i_err = j;
      return -1;
    }
    __json_blob_change_payload_size(parser, i_this, parser->n_blob - i_start);
    parser->i_depth--;
    return j+1;
  }
  case '[': {
    /* parser_t array */
    i_this = parser->n_blob;
    assert( i<=(u_int32_t)parser->n_json );
    __json_blob_append_node(parser, JSONB_ARRAY, parser->n_json - i, 0);
    i_start = parser->n_blob;
    if( parser->oom ) return -1;
    if( ++parser->i_depth > JSON_MAX_DEPTH ){
      parser->i_err = i;
      return -1;
    }
    for(j=i+1;;j++){
      x = __json_translate_text_to_blob(parser, j);
      if( x<=0 ){
        if( x==(-3) ){
          j = parser->i_err;
          if( parser->n_blob!=i_start ) parser->has_nonstd = 1;
          break;
        }
        if( x!=(-1) ) parser->i_err = j;
        return -1;
      }
      j = x;
      if( z[j]==',' ){
        continue;
      }else if( z[j]==']' ){
        break;
      }else{
        if( JSON_ISSPACE(z[j]) ){
          j += 1 + (u_int32_t)strspn(&z[j+1], __json_spaces);
          if( z[j]==',' ){
            continue;
          }else if( z[j]==']' ){
            break;
          }
        }
        x = __json_translate_text_to_blob(parser, j);
        if( x==(-4) ){
          j = parser->i_err;
          continue;
        }
        if( x==(-3) ){
          j = parser->i_err;
          break;
        }
      }
      parser->i_err = j;
      return -1;
    }
    __json_blob_change_payload_size(parser, i_this, parser->n_blob - i_start);
    parser->i_depth--;
    return j+1;
  }
  case '\'': {
    u_int8_t opcode;
    char c_delim;
    parser->has_nonstd = 1;
    opcode = JSONB_TEXT;
    goto parse_string;
  case '"':
    /* parser_t string */
    opcode = JSONB_TEXT;
  parse_string:
    c_delim = z[i];
    j = i+1;
    while( 1 /*exit-by-break*/ ){
      if( __json_is_ok[(u_int8_t)z[j]] ){
        if( !__json_is_ok[(u_int8_t)z[j+1]] ){
          j += 1;
        }else if( !__json_is_ok[(u_int8_t)z[j+2]] ){
          j += 2;
        }else{
          j += 3;
          continue;
        }
      }
      c = z[j];
      if( c==c_delim ){
        break;
      }else if( c=='\\' ){
        c = z[++j];
        if( c=='"' || c=='\\' || c=='/' || c=='b' || c=='f'
           || c=='n' || c=='r' || c=='t'
           || (c=='u' && __json_is4_hex(&z[j+1])) ){
          if( opcode==JSONB_TEXT ) opcode = JSONB_TEXTJ;
        }else if( c=='\'' ||  c=='v' || c=='\n'
#ifdef DBSQL_BUG_COMPATIBLE_20250510
           || (c=='0')                            /* Legacy bug compatible */
#else
           || (c=='0' && !__isdigit(z[j+1])) /* Correct implementation */
#endif
           || (0xe2==(u_int8_t)c && 0x80==(u_int8_t)z[j+1]
                && (0xa8==(u_int8_t)z[j+2] || 0xa9==(u_int8_t)z[j+2]))
           || (c=='x' && __json_is2_hex(&z[j+1])) ){
          opcode = JSONB_TEXT5;
          parser->has_nonstd = 1;
        }else if( c=='\r' ){
          if( z[j+1]=='\n' ) j++;
          opcode = JSONB_TEXT5;
          parser->has_nonstd = 1;
        }else{
          parser->i_err = j;
          return -1;
        }
      }else if( c<=0x1f ){
        if( c==0 ){
          parser->i_err = j;
          return -1;
        }
        /* Control characters are not allowed in canonical JSON string
        ** literals, but are allowed in JSON5 string literals. */
        opcode = JSONB_TEXT5;
        parser->has_nonstd = 1;
      }else if( c=='"' ){
        opcode = JSONB_TEXT5;
      }
      j++;
    }
    __json_blob_append_node(parser, opcode, j-1-i, &z[i+1]);
    return j+1;
  }
  case 't': {
    if( strncmp(z+i,"true",4)==0 && !__isalnum(z[i+4]) ){
      __json_blob_append_one_byte(parser, JSONB_TRUE);
      return i+4;
    }
    parser->i_err = i;
    return -1;
  }
  case 'f': {
    if( strncmp(z+i,"false",5)==0 && !__isalnum(z[i+5]) ){
      __json_blob_append_one_byte(parser, JSONB_FALSE);
      return i+5;
    }
    parser->i_err = i;
    return -1;
  }
  case '+': {
    u_int8_t seen_e;
    parser->has_nonstd = 1;
    t = 0x00;            /* Bit 0x01:  JSON5.   Bit 0x02:  FLOAT */
    goto parse_number;
  case '.':
    if( __isdigit(z[i+1]) ){
      parser->has_nonstd = 1;
      t = 0x03;          /* Bit 0x01:  JSON5.   Bit 0x02:  FLOAT */
      seen_e = 0;
      goto parse_number_2;
    }
    parser->i_err = i;
    return -1;
  case '-':
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    /* parser_t number */
    t = 0x00;            /* Bit 0x01:  JSON5.   Bit 0x02:  FLOAT */
  parse_number:
    seen_e = 0;
    assert( '-' < '0' );
    assert( '+' < '0' );
    assert( '.' < '0' );
    c = z[i];

    if( c<='0' ){
      if( c=='0' ){
        if( (z[i+1]=='x' || z[i+1]=='X') && __isxdigit(z[i+2]) ){
          assert( t==0x00 );
          parser->has_nonstd = 1;
          t = 0x01;
          for(j=i+3; __isxdigit(z[j]); j++){}
          goto parse_number_finish;
        }else if( __isdigit(z[i+1]) ){
          parser->i_err = i+1;
          return -1;
        }
      }else{
        if( !__isdigit(z[i+1]) ){
          /* JSON5 allows for "+Infinity" and "-Infinity" using exactly
          ** that case.  dbsql also allows these in any case and it allows
          ** "+inf" and "-inf". */
          if( (z[i+1]=='I' || z[i+1]=='i')
           && __str_nicmp(&z[i+1], "inf",3)==0
          ){
            parser->has_nonstd = 1;
            if( z[i]=='-' ){
              __json_blob_append_node(parser, JSONB_FLOAT, 6, "-9e999");
            }else{
              __json_blob_append_node(parser, JSONB_FLOAT, 5, "9e999");
            }
            return i + (__str_nicmp(&z[i+4],"inity",5)==0 ? 9 : 4);
          }
          if( z[i+1]=='.' ){
            parser->has_nonstd = 1;
            t |= 0x01;
            goto parse_number_2;
          }
          parser->i_err = i;
          return -1;
        }
        if( z[i+1]=='0' ){
          if( __isdigit(z[i+2]) ){
            parser->i_err = i+1;
            return -1;
          }else if( (z[i+2]=='x' || z[i+2]=='X') && __isxdigit(z[i+3]) ){
            parser->has_nonstd = 1;
            t |= 0x01;
            for(j=i+4; __isxdigit(z[j]); j++){}
            goto parse_number_finish;
          }
        }
      }
    }

  parse_number_2:
    for(j=i+1;; j++){
      c = z[j];
      if( __isdigit(c) ) continue;
      if( c=='.' ){
        if( (t & 0x02)!=0 ){
          parser->i_err = j;
          return -1;
        }
        t |= 0x02;
        continue;
      }
      if( c=='e' || c=='E' ){
        if( z[j-1]<'0' ){
          if( ALWAYS(z[j-1]=='.') && ALWAYS(j-2>=i) && __isdigit(z[j-2]) ){
            parser->has_nonstd = 1;
            t |= 0x01;
          }else{
            parser->i_err = j;
            return -1;
          }
        }
        if( seen_e ){
          parser->i_err = j;
          return -1;
        }
        t |= 0x02;
        seen_e = 1;
        c = z[j+1];
        if( c=='+' || c=='-' ){
          j++;
          c = z[j+1];
        }
        if( c<'0' || c>'9' ){
          parser->i_err = j;
          return -1;
        }
        continue;
      }
      break;
    }
    if( z[j-1]<'0' ){
      if( ALWAYS(z[j-1]=='.') && ALWAYS(j-2>=i) && __isdigit(z[j-2]) ){
        parser->has_nonstd = 1;
        t |= 0x01;
      }else{
        parser->i_err = j;
        return -1;
      }
    }
  parse_number_finish:
    assert( JSONB_INT+0x01==JSONB_INT5 );
    assert( JSONB_FLOAT+0x01==JSONB_FLOAT5 );
    assert( JSONB_INT+0x02==JSONB_FLOAT );
    if( z[i]=='+' ) i++;
    __json_blob_append_node(parser, JSONB_INT+t, j-i, &z[i]);
    return j;
  }
  case '}': {
    parser->i_err = i;
    return -2;  /* End of {...} */
  }
  case ']': {
    parser->i_err = i;
    return -3;  /* End of [...] */
  }
  case ',': {
    parser->i_err = i;
    return -4;  /* List separator */
  }
  case ':': {
    parser->i_err = i;
    return -5;  /* Object label/value separator */
  }
  case 0: {
    return 0;   /* End of file */
  }
  case 0x09:
  case 0x0a:
  case 0x0d:
  case 0x20: {
    i += 1 + (u_int32_t)strspn(&z[i+1], __json_spaces);
    goto json_parse_restart;
  }
  case 0x0b:
  case 0x0c:
  case '/':
  case 0xc2:
  case 0xe1:
  case 0xe2:
  case 0xe3:
  case 0xef: {
    j = json5Whitespace(&z[i]);
    if( j>0 ){
      i += j;
      parser->has_nonstd = 1;
      goto json_parse_restart;
    }
    parser->i_err = i;
    return -1;
  }
  case 'n': {
    if( strncmp(z+i,"null",4)==0 && !__isalnum(z[i+4]) ){
      __json_blob_append_one_byte(parser, JSONB_NULL);
      return i+4;
    }
    /* fall-through into the default case that checks for __na_n */
    /* no break */ deliberate_fall_through
  }
  default: {
    u_int32_t k;
    int nn;
    c = z[i];
    for(k=0; k<sizeof(__a_nan_inf_name)/sizeof(__a_nan_inf_name[0]); k++){
      if( c!=__a_nan_inf_name[k].c1 && c!=__a_nan_inf_name[k].c2 ) continue;
      nn = __a_nan_inf_name[k].n;
      if( __str_nicmp(&z[i], __a_nan_inf_name[k].match, nn)!=0 ){
        continue;
      }
      if( __isalnum(z[i+nn]) ) continue;
      if( __a_nan_inf_name[k].type==JSONB_FLOAT ){
        __json_blob_append_node(parser, JSONB_FLOAT, 5, "9e999");
      }else{
        __json_blob_append_one_byte(parser, JSONB_NULL);
      }
      parser->has_nonstd = 1;
      return i + nn;
    }
    parser->i_err = i;
    return -1;  /* Syntax error */
  }
  } /* End switch(z[i]) */
}


/*
** parser_t a complete JSON string.  Return 0 on success or non-zero if there
** are any errors.  If an error occurs, free all memory held by parser,
** but not parser itself.
**
** parser must be initialized to an empty parse object prior to calling
** this routine.
*/
static int __json_convert_text_to_blob(
  json_parse_t *parser,           /* Initialize and fill this json_parse_t object */
  dbsql_func_t *ctx        /* Report errors here */
){
  int i;
  const char *json = parser->json;
  i = __json_translate_text_to_blob(parser, 0);
  if( parser->oom ) i = -1;
  if( i>0 ){
#ifdef DBSQL_DEBUG
    assert( parser->i_depth==0 );
    if( __config.json_selfcheck ){
      assert( __jsonb_validity_check(parser, 0, parser->n_blob, 0)==0 );
    }   
#endif
    while( JSON_ISSPACE(json[i]) ) i++;
    if( json[i] ){
      i += json5Whitespace(&json[i]);
      if( json[i] ){
        if( ctx ) dbsql_result_error(ctx, "malformed JSON", -1);
        __json_parse_reset(parser);
        return 1;
      }
      parser->has_nonstd = 1;
    }
  }
  if( i<=0 ){
    if( ctx!=0 ){
      if( parser->oom ){
        dbsql_result_error_nomem(ctx);
      }else{
        dbsql_result_error(ctx, "malformed JSON", -1);
      }
    }
    __json_parse_reset(parser);
    return 1;
  }
  return 0;
}

/*
** The input string str is a well-formed JSON text string.  Convert
** this into the JSONB format and make it the return value of the
** SQL function.
*/
static void __json_return_string_as_blob(json_string_t *str){
  json_parse_t px;
  assert( str->err==0 );
  memset(&px, 0, sizeof(px));
  px.json = str->buf;
  px.n_json = str->n_used;
  px.db = dbsql_context_db_handle(str->ctx);
  (void)__json_translate_text_to_blob(&px, 0);
  if( px.oom ){
    __dbsql_db_free(px.db, px.blobs);
    dbsql_result_error_nomem(str->ctx);
  }else{
    assert( px.n_blob_alloc>0 );
    assert( !px.read_only );
    dbsql_result_blob(str->ctx, px.blobs, px.n_blob, DBSQL_DYNAMIC);
  }
}

/* The byte at index i is a node type-code.  This routine
** determines the payload size for that node and writes that
** payload size in to *sz.  It returns the offset from i to the
** beginning of the payload.  Return 0 on error.
*/
static u_int32_t __jsonb_payload_size(const json_parse_t *parser, u_int32_t i, u_int32_t *p_sz){
  u_int8_t x;
  u_int32_t sz;
  u_int32_t n;
  if( i>=parser->n_blob ){
    *p_sz = 0;
    return 0;
  }else if( (x = parser->blobs[i]>>4)<=11 ){
    sz = x;
    n = 1;
  }else if( x==12 ){
    if( i+1>=parser->n_blob ){
      *p_sz = 0;
      return 0;
    }
    sz = parser->blobs[i+1];
    n = 2;
  }else if( x==13 ){
    if( i+2>=parser->n_blob ){
      *p_sz = 0;
      return 0;
    }
    sz = (parser->blobs[i+1]<<8) + parser->blobs[i+2];
    n = 3;
  }else if( x==14 ){
    if( i+4>=parser->n_blob ){
      *p_sz = 0;
      return 0;
    }
    sz = ((u_int32_t)parser->blobs[i+1]<<24) + (parser->blobs[i+2]<<16) +
         (parser->blobs[i+3]<<8) + parser->blobs[i+4];
    n = 5;
  }else{
    if( i+8>=parser->n_blob
     || parser->blobs[i+1]!=0
     || parser->blobs[i+2]!=0
     || parser->blobs[i+3]!=0
     || parser->blobs[i+4]!=0
    ){
      *p_sz = 0;
      return 0;
    }
    sz = ((u_int32_t)parser->blobs[i+5]<<24) + (parser->blobs[i+6]<<16) +
         (parser->blobs[i+7]<<8) + parser->blobs[i+8];
    n = 9;
  }
  if( (int64_t)i+sz+n > parser->n_blob
   && (int64_t)i+sz+n > parser->n_blob-parser->delta
  ){
    *p_sz = 0;
    return 0;
  }
  *p_sz = sz;
  return n;
}


/*
** Translate the binary JSONB representation of JSON beginning at
** parser->blobs[i] into a JSON text string.  Append the JSON
** text onto the end of out.  Return the index in parser->blobs[]
** of the first byte past the end of the element that is translated.
**
** If an error is detected in the BLOB input, the out->err flag
** might get set to JSTRING_MALFORMED.  But not all BLOB input errors
** are detected.  So a malformed JSONB input might either result
** in an error, or in incorrect JSON.
**
** The out->err JSTRING_OOM flag is set on a OOM.
*/
static u_int32_t __json_translate_blob_to_text(
  json_parse_t *parser,             /* the complete parse of the JSON */
  u_int32_t i,                         /* Start rendering at this index */
  json_string_t *out               /* Write JSON here */
){
  u_int32_t sz, n, j, i_end;

  n = __jsonb_payload_size(parser, i, &sz);
  if( n==0 ){
    out->err |= JSTRING_MALFORMED;
    return parser->n_blob+1;
  }
  switch( parser->blobs[i] & 0x0f ){
    case JSONB_NULL: {
      __json_append_raw_nz(out, "null", 4);
      return i+1;
    }
    case JSONB_TRUE: {
      __json_append_raw_nz(out, "true", 4);
      return i+1;
    }
    case JSONB_FALSE: {
      __json_append_raw_nz(out, "false", 5);
      return i+1;
    }
    case JSONB_INT:
    case JSONB_FLOAT: {
      if( sz==0 ) goto malformed_jsonb;
      __json_append_raw(out, (const char*)&parser->blobs[i+n], sz);
      break;
    }
    case JSONB_INT5: {  /* Integer literal in hexadecimal notation */
      u_int32_t k = 2;
      u_int64_t u = 0;
      const char *in = (const char*)&parser->blobs[i+n];
      int overflow = 0;
      if( sz==0 ) goto malformed_jsonb;
      if( in[0]=='-' ){
        __json_append_char(out, '-');
        k++;
      }else if( in[0]=='+' ){
        k++;
      }
      for(; k<sz; k++){
        if( !__isxdigit(in[k]) ){
          out->err |= JSTRING_MALFORMED;
          break;
        }else if( (u>>60)!=0 ){
          overflow = 1;
        }else{
          u = u*16 + __hex_to_int(in[k]);
        }
      }
      __json_printf(100,out,overflow?"9.0e999":"%llu", u);
      break;
    }
    case JSONB_FLOAT5: { /* Float literal missing digits beside "." */
      u_int32_t k = 0;
      const char *in = (const char*)&parser->blobs[i+n];
      if( sz==0 ) goto malformed_jsonb;
      if( in[0]=='-' ){
        __json_append_char(out, '-');
        if( sz<=1 ) goto malformed_jsonb;
        k = 1;
      }
      if( in[k]=='.' ){
        __json_append_char(out, '0');
      }
      for(; k<sz; k++){
        __json_append_char(out, in[k]);
        if( in[k]=='.' && (k+1==sz || !__isdigit(in[k+1])) ){
          __json_append_char(out, '0');
        }
      }
      break;
    }
    case JSONB_TEXT:
    case JSONB_TEXTJ: {
      if( out->n_used+sz+2<=out->n_alloc || __json_string_grow(out, sz+2)==0 ){
        out->buf[out->n_used] = '"';
        memcpy(out->buf+out->n_used+1,(const char*)&parser->blobs[i+n],sz);
        out->buf[out->n_used+sz+1] = '"';
        out->n_used += sz+2;
      }
      break;
    }
    case JSONB_TEXT5: {
      const char *in;
      u_int32_t k;
      u_int32_t sz2 = sz;
      in = (const char*)&parser->blobs[i+n];
      __json_append_char(out, '"');
      while( sz2>0 ){
        for(k=0; k<sz2 && (__json_is_ok[(u_int8_t)in[k]] || in[k]=='\''); k++){}
        if( k>0 ){
          __json_append_raw_nz(out, in, k);
          if( k>=sz2 ){
            break;
          }
          in += k;
          sz2 -= k;
        }
        if( in[0]=='"' ){
          __json_append_raw_nz(out, "\\\"", 2);
          in++;
          sz2--;
          continue;
        }
        if( in[0]<=0x1f ){
          if( out->n_used+7>out->n_alloc && __json_string_grow(out,7) ) break;
          __json_append_control_char(out, in[0]);
          in++;
          sz2--;
          continue;
        }
        assert( in[0]=='\\' );
        assert( sz2>=1 );
        if( sz2<2 ){
          out->err |= JSTRING_MALFORMED;
          break;
        }
        switch( (u_int8_t)in[1] ){
          case '\'':
            __json_append_char(out, '\'');
            break;
          case 'v':
            __json_append_raw_nz(out, "\\u000b", 6);
            break;
          case 'x':
            if( sz2<4 ){
              out->err |= JSTRING_MALFORMED;
              sz2 = 2;
              break;
            }
            __json_append_raw_nz(out, "\\u00", 4);
            __json_append_raw_nz(out, &in[2], 2);
            in += 2;
            sz2 -= 2;
            break;
          case '0':
            __json_append_raw_nz(out, "\\u0000", 6);
            break;
          case '\r':
            if( sz2>2 && in[2]=='\n' ){
              in++;
              sz2--;
            }
            break;
          case '\n':
            break;
          case 0xe2:
            /* '\' followed by either U+2028 or U+2029 is ignored as
            ** whitespace.  Not that in UTF8, U+2028 is 0xe2 0x80 0x29.
            ** U+2029 is the same except for the last byte */
            if( sz2<4
             || 0x80!=(u_int8_t)in[2]
             || (0xa8!=(u_int8_t)in[3] && 0xa9!=(u_int8_t)in[3])
            ){
              out->err |= JSTRING_MALFORMED;
              sz2 = 2;
              break;
            }
            in += 2;
            sz2 -= 2;
            break;
          default:
            __json_append_raw_nz(out, in, 2);
            break;
        }
        assert( sz2>=2 );
        in += 2;
        sz2 -= 2;
      }
      __json_append_char(out, '"');
      break;
    }
    case JSONB_TEXTRAW: {
      __json_append_string(out, (const char*)&parser->blobs[i+n], sz);
      break;
    }
    case JSONB_ARRAY: {
      __json_append_char(out, '[');
      j = i+n;
      i_end = j+sz;
      if( ++parser->i_depth > JSON_MAX_DEPTH ){
        __json_string_too_deep(out);
      }
      while( j<i_end && out->err==0 ){
        j = __json_translate_blob_to_text(parser, j, out);
        __json_append_char(out, ',');
      }
      parser->i_depth--;
      if( j>i_end ) out->err |= JSTRING_MALFORMED;
      if( sz>0 ) __json_string_trim_one_char(out);
      __json_append_char(out, ']');
      break;
    }
    case JSONB_OBJECT: {
      int x = 0;
      __json_append_char(out, '{');
      j = i+n;
      i_end = j+sz;
      if( ++parser->i_depth > JSON_MAX_DEPTH ){
        __json_string_too_deep(out);
      }
      while( j<i_end && out->err==0 ){
        j = __json_translate_blob_to_text(parser, j, out);
        __json_append_char(out, (x++ & 1) ? ',' : ':');
      }
      parser->i_depth--;
      if( (x & 1)!=0 || j>i_end ) out->err |= JSTRING_MALFORMED;
      if( sz>0 ) __json_string_trim_one_char(out);
      __json_append_char(out, '}');
      break;
    }

    default: {
      malformed_jsonb:
      out->err |= JSTRING_MALFORMED;
      break;
    }
  }
  return i+n+sz;
}

/* Context for recursion of json_pretty()
*/
typedef struct json_pretty_t json_pretty_t;
struct json_pretty_t {
  json_parse_t *parser;        /* The BLOB being rendered */
  json_string_t *out;         /* Generate pretty output into this string */
  const char *indent;      /* Use this text for indentation */
  u_int32_t sz_indent;             /* Bytes in indent[] */
  u_int32_t n_indent;              /* Current level of indentation */
};

/* Append indentation to the pretty JSON under construction */
static void __json_pretty_indent(json_pretty_t *pretty){
  u_int32_t jj;
  for(jj=0; jj<pretty->n_indent; jj++){
    __json_append_raw(pretty->out, pretty->indent, pretty->sz_indent);
  }
}

/*
** Translate the binary JSONB representation of JSON beginning at
** parser->blobs[i] into a JSON text string.  Append the JSON
** text onto the end of out.  Return the index in parser->blobs[]
** of the first byte past the end of the element that is translated.
**
** This is a variant of __json_translate_blob_to_text() that "pretty-prints"
** the output.  Extra whitespace is inserted to make the JSON easier
** for humans to read.
**
** If an error is detected in the BLOB input, the out->err flag
** might get set to JSTRING_MALFORMED.  But not all BLOB input errors
** are detected.  So a malformed JSONB input might either result
** in an error, or in incorrect JSON.
**
** The out->err JSTRING_OOM flag is set on a OOM.
*/
static u_int32_t __json_translate_blob_to_pretty_text(
  json_pretty_t *pretty,       /* Pretty-printing context */
  u_int32_t i                      /* Start rendering at this index */
){
  u_int32_t sz, n, j, i_end;
  json_parse_t *parser = pretty->parser;
  json_string_t *out = pretty->out;
  n = __jsonb_payload_size(parser, i, &sz);
  if( n==0 ){
    out->err |= JSTRING_MALFORMED;
    return parser->n_blob+1;
  }
  switch( parser->blobs[i] & 0x0f ){
    case JSONB_ARRAY: {
      j = i+n;
      i_end = j+sz;
      __json_append_char(out, '[');
      if( j<i_end ){
        __json_append_char(out, '\n');
        pretty->n_indent++;
        if( pretty->n_indent >= JSON_MAX_DEPTH ){
          __json_string_too_deep(out);
        }
        while( out->err==0 ){
          __json_pretty_indent(pretty);
          j = __json_translate_blob_to_pretty_text(pretty, j);
          if( j>=i_end ) break;
          __json_append_raw_nz(out, ",\n", 2);
        }
        __json_append_char(out, '\n');
        pretty->n_indent--;
        __json_pretty_indent(pretty);
      }
      __json_append_char(out, ']');
      i = i_end;
      break;
    }
    case JSONB_OBJECT: {
      j = i+n;
      i_end = j+sz;
      __json_append_char(out, '{');
      if( j<i_end ){
        __json_append_char(out, '\n');
        pretty->n_indent++;
        if( pretty->n_indent >= JSON_MAX_DEPTH ){
          __json_string_too_deep(out);
        }
        parser->i_depth = pretty->n_indent;
        while( out->err==0 ){
          __json_pretty_indent(pretty);
          j = __json_translate_blob_to_text(parser, j, out);
          if( j>i_end ){
            out->err |= JSTRING_MALFORMED;
            break;
          }
          __json_append_raw_nz(out, ": ", 2);
          j = __json_translate_blob_to_pretty_text(pretty, j);
          if( j>=i_end ) break;
          __json_append_raw_nz(out, ",\n", 2);
        }
        __json_append_char(out, '\n');
        pretty->n_indent--;
        __json_pretty_indent(pretty);
      }
      __json_append_char(out, '}');
      i = i_end;
      break;
    }
    default: {
      i = __json_translate_blob_to_text(parser, i, out);
      break;
    }
  }
  return i;
}

/*
** Given that a JSONB_ARRAY object starts at offset i, return
** the number of entries in that array.
*/
static u_int32_t __jsonb_array_count(json_parse_t *parser, u_int32_t i_root){
  u_int32_t n, sz, i, i_end;
  u_int32_t k = 0;
  n = __jsonb_payload_size(parser, i_root, &sz);
  i_end = i_root+n+sz;
  for(i=i_root+n; n>0 && i<i_end; i+=sz+n, k++){
    n = __jsonb_payload_size(parser, i, &sz);
  }
  return k;
}

/*
** Edit the payload size of the element at i_root by the amount in
** parser->delta.
*/
static void __json_after_edit_size_adjust(json_parse_t *parser, u_int32_t i_root){
  u_int32_t sz = 0;
  u_int32_t n_blob;
  assert( parser->delta!=0 );
  assert( parser->n_blob_alloc >= parser->n_blob );
  n_blob = parser->n_blob;
  parser->n_blob = parser->n_blob_alloc;
  (void)__jsonb_payload_size(parser, i_root, &sz);
  parser->n_blob = n_blob;
  sz += parser->delta;
  parser->delta += __json_blob_change_payload_size(parser, i_root, sz);
}

/*
** If the JSONB at inses[0..n_ins-1] can be expanded (by denormalizing the
** size field) by d bytes, then write the expansion into outs[] and
** return true.  In this way, an overwrite happens without changing the
** size of the JSONB, which reduces memcpy() operations and also make it
** faster and easier to update the B-Tree entry that contains the JSONB
** in the database.
**
** If the expansion of inses[] by d bytes cannot be (easily) accomplished
** then return false.
**
** The d parameter is guaranteed to be between 1 and 8.
**
** This routine is an optimization.  A correct answer is obtained if it
** always leaves the output unchanged and returns false.
*/
static int __json_blob_overwrite(
  u_int8_t *outs,                 /* Overwrite here */
  const u_int8_t *inses,           /* New content */
  u_int32_t n_ins,                 /* Bytes of new content */
  u_int32_t d                     /* Need to expand new content by this much */
){
  u_int32_t sz_payload;       /* Bytes of payload */
  u_int32_t i;               /* New header size, after expansion & a loop counter */
  u_int8_t sz_hdr;            /* Size of header before expansion */

  /* Lookup table for finding the upper 4 bits of the first byte of the
  ** expanded inses[], based on the size of the expanded inses[] header:
  **
  **                             2     3  4     5  6  7  8     9 */
  static const u_int8_t types[] = { 0xc0, 0xd0, 0, 0xe0, 0, 0, 0, 0xf0 };

  if( (inses[0]&0x0f)<=2 ) return 0;    /* Cannot enlarge NULL, true, false */
  switch( inses[0]>>4 ){
    default: {                         /* inses[] header size 1 */
      if( ((1<<d)&0x116)==0 ) return 0;  /* d must be 1, 2, 4, or 8 */
      i = d + 1;                         /* New hdr sz: 2, 3, 5, or 9 */
      sz_hdr = 1;
      break;
    }
    case 12: {                         /* inses[] header size is 2 */
      if( ((1<<d)&0x8a)==0) return 0;    /* d must be 1, 3, or 7 */
      i = d + 2;                         /* New hdr sz: 2, 5, or 9 */
      sz_hdr = 2;
      break;
    }
    case 13: {                         /* inses[] header size is 3 */
      if( d!=2 && d!=6 ) return 0;       /* d must be 2 or 6 */
      i = d + 3;                         /* New hdr sz: 5 or 9 */
      sz_hdr = 3;
      break;
    }
    case 14: {                         /* inses[] header size is 5 */
      if( d!=4 ) return 0;               /* d must be 4 */
      i = 9;                             /* New hdr sz: 9 */
      sz_hdr = 5;
      break;
    }
    case 15: {                         /* inses[] header size is 9 */
      return 0;                          /* No solution */
    }
  }
  assert( i>=2 && i<=9 && types[i-2]!=0 );
  outs[0] = (inses[0] & 0x0f) | types[i-2];
  memcpy(&outs[i], &inses[sz_hdr], n_ins-sz_hdr);
  sz_payload = n_ins - sz_hdr;
  while( 1/*edit-by-break*/ ){
    i--;
    outs[i] = sz_payload & 0xff;
    if( i==1 ) break;
    sz_payload >>= 8;
  }
  assert( (sz_payload>>8)==0 );
  return 1;
}

/*
** Modify the JSONB blob at parser->blobs by removing n_del bytes of
** content beginning at i_del, and replacing them with n_ins bytes of
** content given by inses.
**
** n_del may be zero, in which case no bytes are removed.  But i_del is
** still important as new bytes will be insert beginning at i_del.
**
** inses may be zero, in which case space is created to hold n_ins bytes
** beginning at i_del, but that space is uninitialized.
**
** Set parser->oom if an OOM occurs.
*/
static void __json_blob_edit(
  json_parse_t *parser,     /* The JSONB to be modified is in parser->blobs */
  u_int32_t i_del,              /* First byte to be removed */
  u_int32_t n_del,              /* Number of bytes to remove */
  const u_int8_t *inses,        /* Content to insert */
  u_int32_t n_ins               /* Bytes of content to insert */
){
  int64_t d = (int64_t)n_ins - (int64_t)n_del;
  assert( parser->n_blob >= (u_int64_t)i_del + (u_int64_t)n_del );
  if( d<0 && d>=(-8) && inses!=0
   && __json_blob_overwrite(&parser->blobs[i_del], inses, n_ins, (int)-d)
  ){
    return;
  }
  if( d!=0 ){
    if( parser->n_blob + d > parser->n_blob_alloc ){
      __json_blob_expand(parser, parser->n_blob+d);
      if( parser->oom ) return;
    }
    memmove(&parser->blobs[i_del+n_ins],
            &parser->blobs[i_del+n_del],
            parser->n_blob - (i_del+n_del));
    parser->n_blob += d;
    parser->delta += d;
  }
  if( n_ins && inses ){
    memcpy(&parser->blobs[i_del], inses, n_ins);
  }
}

/*
** Return the number of escaped newlines to be ignored.
** An escaped newline is a one of the following byte sequences:
**
**    0x5c 0x0a
**    0x5c 0x0d
**    0x5c 0x0d 0x0a
**    0x5c 0xe2 0x80 0xa8
**    0x5c 0xe2 0x80 0xa9
*/
static u_int32_t __json_bytes_to_bypass(const char *z, u_int32_t n){
  u_int32_t i = 0;
  while( i+1<n ){
    if( z[i]!='\\' ) return i;
    if( z[i+1]=='\n' ){
      i += 2;
      continue;
    }
    if( z[i+1]=='\r' ){
      if( i+2<n && z[i+2]=='\n' ){
        i += 3;
      }else{
        i += 2;
      }
      continue;
    }
    if( 0xe2==(u_int8_t)z[i+1]
     && i+3<n
     && 0x80==(u_int8_t)z[i+2]
     && (0xa8==(u_int8_t)z[i+3] || 0xa9==(u_int8_t)z[i+3])
    ){
      i += 4;
      continue;
    }
    break;
  }
  return i;
}

/*
** Input z[0..n] defines JSON escape sequence including the leading '\\'.
** Decode that escape sequence into a single character.  Write that
** character into *pi_out.  Return the number of bytes in the escape sequence.
**
** If there is a syntax error of some kind (for example too few characters
** after the '\\' to complete the encoding) then *pi_out is set to
** JSON_INVALID_CHAR.
*/
static u_int32_t __json_unescape_one_char(const char *z, u_int32_t n, u_int32_t *pi_out){
  assert( n>0 );
  assert( z[0]=='\\' );
  if( n<2 ){
    *pi_out = JSON_INVALID_CHAR;
    return n;
  }
  switch( (u_int8_t)z[1] ){
    case 'u': {
      u_int32_t v, vlo;
      if( n<6 ){
        *pi_out = JSON_INVALID_CHAR;
        return n;
      }
      v = __json_hex_to_int4(&z[2]);
      if( (v & 0xfc00)==0xd800
       && n>=12
       && z[6]=='\\'
       && z[7]=='u'
       && ((vlo = __json_hex_to_int4(&z[8]))&0xfc00)==0xdc00
      ){
        *pi_out = ((v&0x3ff)<<10) + (vlo&0x3ff) + 0x10000;
        return 12;
      }else{
        *pi_out = v;
        return 6;
      }
    }
    case 'b': {   *pi_out = '\b';  return 2; }
    case 'f': {   *pi_out = '\f';  return 2; }
    case 'n': {   *pi_out = '\n';  return 2; }
    case 'r': {   *pi_out = '\r';  return 2; }
    case 't': {   *pi_out = '\t';  return 2; }
    case 'v': {   *pi_out = '\v';  return 2; }
    case '0': {
      /* JSON5 requires that the \0 escape not be followed by a digit.
      ** But dbsql did not enforce this restriction in versions 3.42.0
      ** through 3.49.2.  That was a bug.  But some applications might have
      ** come to depend on that bug.  Use the DBSQL_BUG_COMPATIBLE_20250510
      ** option to restore the old buggy behavior. */
#ifdef DBSQL_BUG_COMPATIBLE_20250510
      /* Legacy bug-compatible behavior */
      *pi_out = 0;
#else
      /* Correct behavior */
      *pi_out = (n>2 && __isdigit(z[2])) ? JSON_INVALID_CHAR : 0;
#endif
      return 2;
    }
    case '\'':
    case '"':
    case '/':
    case '\\':{   *pi_out = z[1];  return 2; }
    case 'x': {
      if( n<4 ){
        *pi_out = JSON_INVALID_CHAR;
        return n;
      }
      *pi_out = (__json_hex_to_int(z[2])<<4) | __json_hex_to_int(z[3]);
      return 4;
    }
    case 0xe2:
    case '\r':
    case '\n': {
      u_int32_t n_skip = __json_bytes_to_bypass(z, n);
      if( n_skip==0 ){
        *pi_out = JSON_INVALID_CHAR;
        return n;
      }else if( n_skip==n ){
        *pi_out = 0;
        return n;
      }else if( z[n_skip]=='\\' ){
        return n_skip + __json_unescape_one_char(&z[n_skip], n-n_skip, pi_out);
      }else{
        int sz = __utf8_read_limited((u_int8_t*)&z[n_skip], n-n_skip, pi_out);
        return n_skip + sz;
      }
    }
    default: {
      *pi_out = JSON_INVALID_CHAR;
      return 2;
    }
  }
}


/*
** Compare two object labels.  Return 1 if they are equal and
** 0 if they differ.
**
** In this version, we know that one or the other or both of the
** two comparands contains an escape sequence.
*/
static DBSQL_NOINLINE int __json_label_compare_escaped(
  const char *left,          /* The left label */
  u_int32_t n_left,                  /* Size of the left label in bytes */
  int raw_left,                /* True if left contains no escapes */
  const char *right,         /* The right label */
  u_int32_t n_right,                 /* Size of the right label in bytes */
  int raw_right                /* True if right is escape-free */
){
  u_int32_t c_left, c_right;
  assert( raw_left==0 || raw_right==0 );
  while( 1 /*exit-by-return*/ ){
    if( n_left==0 ){
      c_left = 0;
    }else if( raw_left || left[0]!='\\' ){
      c_left = ((u_int8_t*)left)[0];
      if( c_left>=0xc0 ){
        int sz = __utf8_read_limited((u_int8_t*)left, n_left, &c_left);
        left += sz;
        n_left -= sz;
      }else{
        left++;
        n_left--;
      }
    }else{
      u_int32_t n = __json_unescape_one_char(left, n_left, &c_left);
      left += n;
      assert( n<=n_left );
      n_left -= n;
    }
    if( n_right==0 ){
      c_right = 0;
    }else if( raw_right || right[0]!='\\' ){
      c_right = ((u_int8_t*)right)[0];
      if( c_right>=0xc0 ){
        int sz = __utf8_read_limited((u_int8_t*)right, n_right, &c_right);
        right += sz;
        n_right -= sz;
      }else{
        right++;
        n_right--;
      }
    }else{
      u_int32_t n = __json_unescape_one_char(right, n_right, &c_right);
      right += n;
      assert( n<=n_right );
      n_right -= n;
    }
    if( c_left!=c_right ) return 0;
    if( c_left==0 ) return 1;
  }
}

/*
** Compare two object labels.  Return 1 if they are equal and
** 0 if they differ.  Return -1 if an OOM occurs.
*/
static int __json_label_compare(
  const char *left,          /* The left label */
  u_int32_t n_left,                  /* Size of the left label in bytes */
  int raw_left,                /* True if left contains no escapes */
  const char *right,         /* The right label */
  u_int32_t n_right,                 /* Size of the right label in bytes */
  int raw_right                /* True if right is escape-free */
){
  if( raw_left && raw_right ){
    /* Simpliest case:  Neither label contains escapes.  A simple
    ** memcmp() is sufficient. */
    if( n_left!=n_right ) return 0;
    return memcmp(left, right, n_left)==0;
  }else{
    return __json_label_compare_escaped(left, n_left, raw_left,
                                   right, n_right, raw_right);
  }
}

/*
** Error returns from __json_lookup_step()
*/
#define JSON_LOOKUP_ERROR      0xffffffff
#define JSON_LOOKUP_NOTFOUND   0xfffffffe
#define JSON_LOOKUP_NOTARRAY   0xfffffffd
#define JSON_LOOKUP_TOODEEP    0xfffffffc
#define JSON_LOOKUP_PATHERROR  0xfffffffb
#define JSON_LOOKUP_ISERROR(x) ((x)>=JSON_LOOKUP_PATHERROR)

/* Forward declaration */
static u_int32_t __json_lookup_step(json_parse_t*,u_int32_t,const char*,u_int32_t);


/* This helper routine for __json_lookup_step() populates ins with
** binary data that is to be inserted into parser.
**
** In the common case, ins just points to parser->inses and parser->n_ins.
** But if the path of the original edit operation includes path elements
** that go deeper, additional substructure must be created.
**
** For example:
**
**     json_insert('{}', '$.a.b.c', 123);
**
** The search stops at '$.a'  But additional substructure must be
** created for the ".b.c" part of the patch so that the final result
** is:  {"a":{"b":{"c"::123}}}.  This routine populates ins with
** the binary equivalent of {"b":{"c":123}} so that it can be inserted.
**
** The caller is responsible for resetting ins when it has finished
** using the substructure.
*/
static u_int32_t __json_create_edit_substructure(
  json_parse_t *parser,  /* The original JSONB that is being edited */
  json_parse_t *ins,    /* Populate this with the blob data to insert */
  const char *tail   /* Tail of the path that determines substructure */
){
  static const u_int8_t empty_object[] = { JSONB_ARRAY, JSONB_OBJECT };
  int rc;
  memset(ins, 0, sizeof(*ins));
  ins->db = parser->db;
  if( tail[0]==0 ){
    /* No substructure.  Just insert what is given in parser. */
    ins->blobs = parser->inses;
    ins->n_blob = parser->n_ins;
    rc = 0;
  }else{
    /* Construct the binary substructure */
    ins->n_blob = 1;
    ins->blobs = (u_int8_t*)&empty_object[tail[0]=='.'];
    ins->edit = parser->edit;
    ins->n_ins = parser->n_ins;
    ins->inses = parser->inses;
    ins->i_depth = parser->i_depth+1;
    if( ins->i_depth >= JSON_MAX_DEPTH ){
      return JSON_LOOKUP_TOODEEP;
    }
    rc = __json_lookup_step(ins, 0, tail, 0);
    parser->i_depth--;
    parser->oom |= ins->oom;
  }
  return rc;  /* Error code only */
}

/*
** Search along path to find the Json element specified.  Return an
** index into parser->blobs[] for the start of that element's value.
**
** If the value found by this routine is the value half of label/value pair
** within an object, then set path->i_label to the start of the corresponding
** label, before returning.
**
** Return one of the JSON_LOOKUP error codes if problems are seen.
**
** This routine will also modify the blob.  If parser->edit is one of
** JEDIT_DEL, JEDIT_REPL, JEDIT_INS, JEDIT_SET, or JEDIT_AINS, then changes
** might be made to the selected value. If an edit is performed, then the
** return value does not necessarily point to the select element. If an edit
** is performed, the return value is only useful for detecting error
** conditions.
*/
static u_int32_t __json_lookup_step(
  json_parse_t *parser,      /* The JSON to search */
  u_int32_t i_root,              /* Begin the search at this element of blobs[] */
  const char *path,      /* The path to search */
  u_int32_t i_label              /* Label if i_root is a value of in an object */
){
  u_int32_t i, j, k, n_key, sz, n, i_end, rc;
  const char *key;
  u_int8_t x;

  if( path[0]==0 ){
    if( parser->edit && __json_blob_make_editable(parser, parser->n_ins) ){
      n = __jsonb_payload_size(parser, i_root, &sz);
      sz += n;
      if( parser->edit==JEDIT_DEL ){
        if( i_label>0 ){
          sz += i_root - i_label;
          i_root = i_label;
        }
        __json_blob_edit(parser, i_root, sz, 0, 0);
      }else if( parser->edit==JEDIT_INS ){
        /* Already exists, so json_insert() is a no-op */
      }else if( parser->edit==JEDIT_AINS ){
        /* json_array_insert() */
        if( path[-1]!=']' ){
          return JSON_LOOKUP_NOTARRAY;
        }else{
          __json_blob_edit(parser, i_root, 0, parser->inses, parser->n_ins);
        }
      }else{
        /* json_set() or json_replace() */
        __json_blob_edit(parser, i_root, sz, parser->inses, parser->n_ins);
      }
    }
    parser->i_label = i_label;
    return i_root;
  }
  if( path[0]=='.' ){
    int raw_key = 1;
    x = parser->blobs[i_root];
    path++;
    if( path[0]=='"' ){
      key = path + 1;
      for(i=1; path[i] && path[i]!='"'; i++){
        if( path[i]=='\\' && path[i+1]!=0 ) i++;
      }
      n_key = i-1;
      if( path[i] ){
        i++;
      }else{
        return JSON_LOOKUP_PATHERROR;
      }
      testcase( n_key==0 );
      raw_key = memchr(key, '\\', n_key)==0;
    }else{
      key = path;
      for(i=0; path[i] && path[i]!='.' && path[i]!='['; i++){}
      n_key = i;
      if( n_key==0 ){
        return JSON_LOOKUP_PATHERROR;
      }
    }
    if( (x & 0x0f)!=JSONB_OBJECT ) return JSON_LOOKUP_NOTFOUND;
    n = __jsonb_payload_size(parser, i_root, &sz);
    j = i_root + n;  /* j is the index of a label */
    i_end = j+sz;
    while( j<i_end ){
      int raw_label;
      const char *label;
      x = parser->blobs[j] & 0x0f;
      if( x<JSONB_TEXT || x>JSONB_TEXTRAW ) return JSON_LOOKUP_ERROR;
      n = __jsonb_payload_size(parser, j, &sz);
      if( n==0 ) return JSON_LOOKUP_ERROR;
      k = j+n;  /* k is the index of the label text */
      if( k+sz>=i_end ) return JSON_LOOKUP_ERROR;
      label = (const char*)&parser->blobs[k];
      raw_label = x==JSONB_TEXT || x==JSONB_TEXTRAW;
      if( __json_label_compare(key, n_key, raw_key, label, sz, raw_label) ){
        u_int32_t v = k+sz;  /* v is the index of the value */
        if( ((parser->blobs[v])&0x0f)>JSONB_OBJECT ) return JSON_LOOKUP_ERROR;
        n = __jsonb_payload_size(parser, v, &sz);
        if( n==0 || v+n+sz>i_end ) return JSON_LOOKUP_ERROR;
        assert( j>0 );
        if( ++parser->i_depth >= JSON_MAX_DEPTH ){
          return JSON_LOOKUP_TOODEEP;
        }
        rc = __json_lookup_step(parser, v, &path[i], j);
        parser->i_depth--;
        if( parser->delta ) __json_after_edit_size_adjust(parser, i_root);
        return rc;
      }
      j = k+sz;
      if( ((parser->blobs[j])&0x0f)>JSONB_OBJECT ) return JSON_LOOKUP_ERROR;
      n = __jsonb_payload_size(parser, j, &sz);
      if( n==0 ) return JSON_LOOKUP_ERROR;
      j += n+sz;
    }
    if( j>i_end ) return JSON_LOOKUP_ERROR;
    if( parser->edit>=JEDIT_INS ){
      u_int32_t n_ins;          /* Total bytes to insert (label+value) */
      json_parse_t v;       /* BLOB encoding of the value to be inserted */
      json_parse_t ix;      /* Header of the label to be inserted */
      testcase( parser->edit==JEDIT_INS );
      testcase( parser->edit==JEDIT_SET );
      testcase( parser->edit==JEDIT_AINS );
      if( parser->edit==JEDIT_AINS && dbsql_strglob("*]",&path[i])!=0 ){
        return JSON_LOOKUP_NOTARRAY;
      }
      memset(&ix, 0, sizeof(ix));
      ix.db = parser->db;
      __json_blob_append_node(&ix, raw_key?JSONB_TEXTRAW:JSONB_TEXT5, n_key, 0);
      parser->oom |= ix.oom;
      rc = __json_create_edit_substructure(parser, &v, &path[i]);
      if( !JSON_LOOKUP_ISERROR(rc)
       && __json_blob_make_editable(parser, ix.n_blob+n_key+v.n_blob)
      ){
        assert( !parser->oom );
        n_ins = ix.n_blob + n_key + v.n_blob;
        __json_blob_edit(parser, j, 0, 0, n_ins);
        if( !parser->oom ){
          assert( parser->blobs!=0 ); /* Because parser->oom!=0 */
          assert( ix.blobs!=0 );      /* Because pasre->oom!=0 */
          memcpy(&parser->blobs[j], ix.blobs, ix.n_blob);
          k = j + ix.n_blob;
          memcpy(&parser->blobs[k], key, n_key);
          k += n_key;
          memcpy(&parser->blobs[k], v.blobs, v.n_blob);
          if( ALWAYS(parser->delta) ) __json_after_edit_size_adjust(parser, i_root);
        }
      }
      __json_parse_reset(&v);
      __json_parse_reset(&ix);
      return rc;
    }
  }else if( path[0]=='[' ){
    u_int64_t kk = 0;
    x = parser->blobs[i_root] & 0x0f;
    if( x!=JSONB_ARRAY )  return JSON_LOOKUP_NOTFOUND;
    n = __jsonb_payload_size(parser, i_root, &sz);
    i = 1;
    while( __isdigit(path[i]) ){
      if( kk<0xffffffff ) kk = kk*10 + path[i] - '0';
      /*     ^^^^^^^^^^--- Allow kk to be bigger than any JSON array so that
      ** we get NOTFOUND instead of PATHERROR, without overflowing kk. */
      i++;
    }
    if( i<2 || path[i]!=']' ){
      if( path[1]=='#' ){
        kk = __jsonb_array_count(parser, i_root);
        i = 2;
        if( path[2]=='-' && __isdigit(path[3]) ){
          u_int64_t nn = 0;
          i = 3;
          do{
            if( nn<0xffffffff ) nn = nn*10 + path[i] - '0';
            /*     ^^^^^^^^^^--- Allow nn to be bigger than any JSON array to
            ** get NOTFOUND instead of PATHERROR, without overflowing nn. */
            i++;
          }while( __isdigit(path[i]) );
          if( nn>kk ) return JSON_LOOKUP_NOTFOUND;
          kk -= nn;
        }
        if( path[i]!=']' ){
          return JSON_LOOKUP_PATHERROR;
        }
      }else{
        return JSON_LOOKUP_PATHERROR;
      }
    }
    j = i_root+n;
    i_end = j+sz;
    while( j<i_end ){
      if( kk==0 ){
        if( ++parser->i_depth >= JSON_MAX_DEPTH ){
          return JSON_LOOKUP_TOODEEP;
        }
        rc = __json_lookup_step(parser, j, &path[i+1], 0);
        parser->i_depth--;
        if( parser->delta ) __json_after_edit_size_adjust(parser, i_root);
        return rc;
      }
      kk--;
      n = __jsonb_payload_size(parser, j, &sz);
      if( n==0 ) return JSON_LOOKUP_ERROR;
      j += n+sz;
    }
    if( j>i_end ) return JSON_LOOKUP_ERROR;
    if( kk>0 ) return JSON_LOOKUP_NOTFOUND;
    if( parser->edit>=JEDIT_INS ){
      json_parse_t v;
      testcase( parser->edit==JEDIT_INS );
      testcase( parser->edit==JEDIT_AINS );
      testcase( parser->edit==JEDIT_SET );
      rc = __json_create_edit_substructure(parser, &v, &path[i+1]);
      if( !JSON_LOOKUP_ISERROR(rc)
       && __json_blob_make_editable(parser, v.n_blob)
      ){
        assert( !parser->oom );
        __json_blob_edit(parser, j, 0, v.blobs, v.n_blob);
      }
      __json_parse_reset(&v);
      if( parser->delta ) __json_after_edit_size_adjust(parser, i_root);
      return rc;
    }
  }else{
    return JSON_LOOKUP_PATHERROR; 
  }
  return JSON_LOOKUP_NOTFOUND;
}

/*
** Convert a JSON BLOB into text and make that text the return value
** of an SQL function.
*/
static void __json_return_text_json_from_blob(
  dbsql_func_t *ctx,
  const u_int8_t *blobs,
  u_int32_t n_blob
){
  json_parse_t x;
  json_string_t s;

  if( NEVER(blobs==0) ) return;
  memset(&x, 0, sizeof(x));
  x.blobs = (u_int8_t*)blobs;
  x.n_blob = n_blob;
  __json_string_init(&s, ctx);
  __json_translate_blob_to_text(&x, 0, &s);
  __json_return_string(&s, 0, 0);
}


/*
** Return the value of the BLOB node at index i.
**
** If the value is a primitive, return it as an SQL value.
** If the value is an array or object, return it as either
** JSON text or the BLOB encoding, depending on the mode flag
** as follows:
**
**     mode==0     JSONB if the JSON_B flag is set in userdata or
**                  text if the JSON_B flag is omitted from userdata.
**
**     mode==1     Text
**
**     mode==2     JSONB
*/
static void __json_return_from_blob(
  json_parse_t *parser,          /* Complete JSON parse tree */
  u_int32_t i,                      /* index_t of the node */
  dbsql_func_t *ctx,      /* Return value for this function */
  int mode                   /* Format of return: text of JSONB */
){
  u_int32_t n, sz;
  int rc;
  DBSQL *db = dbsql_context_db_handle(ctx);

  assert( mode>=0 && mode<=2 );
  n = __jsonb_payload_size(parser, i, &sz);
  if( n==0 ){
    dbsql_result_error(ctx, "malformed JSON", -1);
    return;
  }
  switch( parser->blobs[i] & 0x0f ){
    case JSONB_NULL: {
      if( sz ) goto returnfromblob_malformed;
      dbsql_result_null(ctx);
      break;
    }
    case JSONB_TRUE: {
      if( sz ) goto returnfromblob_malformed;
      dbsql_result_int(ctx, 1);
      break;
    }
    case JSONB_FALSE: {
      if( sz ) goto returnfromblob_malformed;
      dbsql_result_int(ctx, 0);
      break;
    }
    case JSONB_INT5:
    case JSONB_INT: {
      int64_t i_res = 0;
      char *z;
      int neg = 0;
      char x;
      if( sz==0 ) goto returnfromblob_malformed;
      x = (char)parser->blobs[i+n];
      if( x=='-' ){
        if( sz<2 ) goto returnfromblob_malformed;
        n++;
        sz--;
        neg = 1;
      }
      z = __db_str_n_dup(db, (const char*)&parser->blobs[i+n], (int)sz);
      if( z==0 ) goto returnfromblob_oom;
      rc = __dec_or_hex_to_i64(z, &i_res);
      __dbsql_db_free(db, z);
      if( rc==0 ){
        if( i_res<0 ){
          /* A hexadecimal literal with 16 significant digits and with the
          ** high-order bit set is a negative integer in dbsql (and hence
          ** i_res comes back as negative) but should be interpreted as a
          ** positive value if it occurs within JSON.  The value is too
          ** large to appear as an dbsql integer so it must be converted
          ** into floating point. */
          double r;
          r = (double)*(u_int64_t*)&i_res;
          dbsql_result_double(ctx, neg ? -r : r);
        }else{
          dbsql_result_int64(ctx, neg ? -i_res : i_res);
        }
      }else if( rc==3 && neg ){
        dbsql_result_int64(ctx, SMALLEST_INT64);
      }else if( rc==1 ){
        goto returnfromblob_malformed;
      }else{
        if( neg ){ n--; sz++; }
        goto to_double;
      }
      break;
    }
    case JSONB_FLOAT5:
    case JSONB_FLOAT: {
      double r;
      char *z;
      if( sz==0 ) goto returnfromblob_malformed;
    to_double:
      z = __db_str_n_dup(db, (const char*)&parser->blobs[i+n], (int)sz);
      if( z==0 ) goto returnfromblob_oom;
      rc = __ato_f(z, &r);
      __dbsql_db_free(db, z);
      if( rc<=0 ) goto returnfromblob_malformed;
      dbsql_result_double(ctx, r);
      break;
    }
    case JSONB_TEXTRAW:
    case JSONB_TEXT: {
      dbsql_result_text(ctx, (char*)&parser->blobs[i+n], sz,
                          DBSQL_TRANSIENT);
      break;
    }
    case JSONB_TEXT5:
    case JSONB_TEXTJ: {
      /* Translate JSON formatted string into raw text */
      u_int32_t i_in, i_out;
      const char *z;
      char *out;
      u_int32_t n_out = sz;
      z = (const char*)&parser->blobs[i+n];
      out = __db_malloc_raw(db, ((u_int64_t)n_out)+1);
      if( out==0 ) goto returnfromblob_oom;
      for(i_in=i_out=0; i_in<sz; i_in++){
        char c = z[i_in];
        if( c=='\\' ){
          u_int32_t v;
          u_int32_t sz_escape = __json_unescape_one_char(&z[i_in], sz-i_in, &v);
          if( v<=0x7f ){
            out[i_out++] = (char)v;
          }else if( v<=0x7ff ){
            assert( sz_escape>=2 );
            out[i_out++] = (char)(0xc0 | (v>>6));
            out[i_out++] = 0x80 | (v&0x3f);
          }else if( v<0x10000 ){
            assert( sz_escape>=3 );
            out[i_out++] = 0xe0 | (v>>12);
            out[i_out++] = 0x80 | ((v>>6)&0x3f);
            out[i_out++] = 0x80 | (v&0x3f);
          }else if( v==JSON_INVALID_CHAR ){
            /* Silently ignore illegal unicode */
          }else{
            assert( sz_escape>=4 );
            out[i_out++] = 0xf0 | (v>>18);
            out[i_out++] = 0x80 | ((v>>12)&0x3f);
            out[i_out++] = 0x80 | ((v>>6)&0x3f);
            out[i_out++] = 0x80 | (v&0x3f);
          }
          i_in += sz_escape - 1;
        }else{
          out[i_out++] = c;
        }
      } /* end for() */
      assert( i_out<=n_out );
      out[i_out] = 0;
      dbsql_result_text(ctx, out, i_out, DBSQL_DYNAMIC);
      break;
    }
    case JSONB_ARRAY:
    case JSONB_OBJECT: {
      if( mode==0 ){
        if( (DBSQL_PTR_TO_INT(dbsql_user_data(ctx)) & JSON_BLOB)!=0 ){
          mode = 2;
        }else{
          mode = 1;
        }
      }
      if( mode==2 ){
        dbsql_result_blob(ctx, &parser->blobs[i], sz+n, DBSQL_TRANSIENT);
      }else{
        __json_return_text_json_from_blob(ctx, &parser->blobs[i], sz+n);
      }
      break;
    }
    default: {
      goto returnfromblob_malformed;
    }
  }
  return;

returnfromblob_oom:
  dbsql_result_error_nomem(ctx);
  return;

returnfromblob_malformed:
  dbsql_result_error(ctx, "malformed JSON", -1);
  return;
}

/*
** arg is a function argument that might be an SQL value or a JSON
** value.  Figure out what it is and encode it as a JSONB blob.
** Return the results in parser.
**
** parser is uninitialized upon entry.  This routine will handle the
** initialization of parser.  The result will be contained in
** parser->blobs and parser->n_blob.  parser->blobs might be dynamically
** allocated (if parser->n_blob_alloc is greater than zero) in which case
** the caller is responsible for freeing the space allocated to parser->blobs
** when it has finished with it.  Or parser->blobs might be a static string
** or a value obtained from dbsql_value_blob(arg).
**
** If the argument is a BLOB that is clearly not a JSONB, then this
** function might set an error message in ctx and return non-zero.
** It might also set an error message and return non-zero on an OOM error.
*/
static int __json_function_arg_to_blob(
  dbsql_func_t *ctx,
  dbsql_value_t *arg,
  json_parse_t *parser
){
  int type = dbsql_value_type(arg);
  static u_int8_t nulls[] = { 0x00 };
  memset(parser, 0, sizeof(parser[0]));
  parser->db = dbsql_context_db_handle(ctx);
  switch( type ){
    default: {
      parser->blobs = nulls;
      parser->n_blob = 1;
      return 0;
    }
    case DBSQL_BLOB: {
      if( !__json_arg_is_jsonb(arg, parser) ){
        dbsql_result_error(ctx, "JSON cannot hold BLOB values", -1);
        return 1;
      }
      break;
    }
    case DBSQL_TEXT: {
      const char *json = (const char*)dbsql_value_text(arg);
      int n_json = dbsql_value_bytes(arg);
      if( json==0 ) return 1;
      if( dbsql_value_subtype(arg)==JSON_SUBTYPE ){
        parser->json = (char*)json;
        parser->n_json = n_json;
        if( __json_convert_text_to_blob(parser, ctx) ){
          dbsql_result_error(ctx, "malformed JSON", -1);
          __dbsql_db_free(parser->db, parser->blobs);
          memset(parser, 0, sizeof(parser[0]));
          return 1;
        }
      }else{
        __json_blob_append_node(parser, JSONB_TEXTRAW, n_json, json);
      }
      break;
    }
    case DBSQL_FLOAT: {
      double r = dbsql_value_double(arg);
      if( NEVER(__is_na_n(r)) ){
        __json_blob_append_node(parser, JSONB_NULL, 0, 0);
      }else{
        int n = dbsql_value_bytes(arg);
        const char *z = (const char*)dbsql_value_text(arg);
        if( z==0 ) return 1;
        if( z[0]=='I' ){
          __json_blob_append_node(parser, JSONB_FLOAT, 5, "9e999");
        }else if( z[0]=='-' && z[1]=='I' ){
          __json_blob_append_node(parser, JSONB_FLOAT, 6, "-9e999");
        }else{
          __json_blob_append_node(parser, JSONB_FLOAT, n, z);
        }
      }
      break;
    }
    case DBSQL_INTEGER: {
      int n = dbsql_value_bytes(arg);
      const char *z = (const char*)dbsql_value_text(arg);
      if( z==0 ) return 1;
      __json_blob_append_node(parser, JSONB_INT, n, z);
      break;
    }
  }
  if( parser->oom ){
    dbsql_result_error_nomem(ctx);
    return 1;
  }else{
    return 0;
  }
}

/*
** Generate a path error.
**
** The specifics of the error are determined by the rc argument.
**
**          rc                        error
**  -----------------       ----------------------
**  JSON_LOOKUP_ARRAY       "not an array"
**  JSON_LOOKUP_TOODEEP     "JSON nested too deep"
**  JSON_LOOKUP_ERROR       "malformed JSON"
**  otherwise...            "bad JSON path"
**
** If ctx is not NULL then push the error message into ctx and return NULL.
** If ctx is NULL, then return the text of the error message.
*/
static char *__json_bad_path_error(
  dbsql_func_t *ctx,     /* The function call containing the error */
  const char *path,        /* The path with the problem */
  int rc                    /* Maybe JSON_LOOKUP_NOTARRAY */
){
  char *msg;
  if( rc==(int)JSON_LOOKUP_NOTARRAY ){
    msg = dbsql_mprintf("not an array element: %Q", path);
  }else if( rc==(int)JSON_LOOKUP_ERROR ){
    msg = dbsql_mprintf("malformed JSON");
  }else if( rc==(int)JSON_LOOKUP_TOODEEP ){
    msg = dbsql_mprintf("JSON path too deep");
  }else{
    msg = dbsql_mprintf("bad JSON path: %Q", path);
  }
  if( ctx==0 ) return msg;
  if( msg ){
    dbsql_result_error(ctx, msg, -1);
    dbsql_free(msg);
  }else{
    dbsql_result_error_nomem(ctx);
  }
  return 0;
}

/* argv[0] is a BLOB that seems likely to be a JSONB.  Subsequent
** arguments come in pairs where each pair contains a JSON path and
** content to insert or set at that patch.  Do the updates
** and return the result.
**
** The specific operation is determined by edit, which can be one
** of JEDIT_INS, JEDIT_REPL, JEDIT_SET, or JEDIT_AINS.
*/
static void __json_insert_into_blob(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv,
  int edit                /* JEDIT_INS, JEDIT_REPL, JEDIT_SET, JEDIT_AINS */
){
  int i;
  u_int32_t rc = 0;
  const char *path = 0;
  int flgs;
  json_parse_t *p;
  json_parse_t ax;

  assert( (argc&1)==1 );
  flgs = argc==1 ? 0 : JSON_EDITABLE;
  p = __json_parse_func_arg(ctx, argv[0], flgs);
  if( p==0 ) return;
  for(i=1; i<argc-1; i+=2){
    if( dbsql_value_type(argv[i])==DBSQL_NULL ) continue;
    path = (const char*)dbsql_value_text(argv[i]);
    if( path==0 ){
      dbsql_result_error_nomem(ctx);
      __json_parse_free(p);
      return;
    }
    if( path[0]!='$' ) goto json_insert_into_blob_patherror;
    if( __json_function_arg_to_blob(ctx, argv[i+1], &ax) ){
      __json_parse_reset(&ax);
      __json_parse_free(p);
      return;
    }
    if( path[1]==0 ){
      if( edit==JEDIT_REPL || edit==JEDIT_SET ){
        __json_blob_edit(p, 0, p->n_blob, ax.blobs, ax.n_blob);
      }
      rc = 0;
   }else{
      p->edit = edit;
      p->n_ins = ax.n_blob;
      p->inses = ax.blobs;
      p->delta = 0;
      p->i_depth = 0;
      rc = __json_lookup_step(p, 0, path+1, 0);
    }
    __json_parse_reset(&ax);
    if( rc==JSON_LOOKUP_NOTFOUND ) continue;
    if( JSON_LOOKUP_ISERROR(rc) ) goto json_insert_into_blob_patherror;
  }
  __json_return_parse(ctx, p);
  __json_parse_free(p);
  return;

json_insert_into_blob_patherror:
  __json_parse_free(p);
  __json_bad_path_error(ctx, path, rc);
  return;
}

/*
** If arg is a blob that seems like a JSONB blob, then initialize
** p to point to that JSONB and return TRUE.  If arg does not seem like
** a JSONB blob, then return FALSE.
**
** For small BLOBs (having no more than 7 bytes of payload) a full
** validity check is done.  So for small BLOBs this routine only returns
** true if the value is guaranteed to be a valid JSONB.  For larger BLOBs
** (8 byte or more of payload) only the size of the outermost element is
** checked to verify that the BLOB is superficially valid JSONB.
**
** A full JSONB validation is done on smaller BLOBs because those BLOBs might
** also be text JSON that has been incorrectly cast into a BLOB.
** (See tag-20240123-a and https://sqlite.org/forum/forumpost/012136abd5)
** If the BLOB is 9 bytes are larger, then it is not possible for the
** superficial size check done here to pass if the input is really text
** JSON so we do not need to look deeper in that case.
**
** Why we only need to do full JSONB validation for smaller BLOBs:
**
** The first byte of valid JSON text must be one of: '{', '[', '"', ' ', '\n',
** '\r', '\t', '-', or a digit '0' through '9'.  Of these, only a subset
** can also be the first byte of JSONB:  '{', '[', and digits '3'
** through '9'.  In every one of those cases, the payload size is 7 bytes
** or less.  So if we do full JSONB validation for every BLOB where the
** payload is less than 7 bytes, we will never get a false positive for
** JSONB on an input that is really text JSON.
*/
static int __json_arg_is_jsonb(dbsql_value_t *arg, json_parse_t *p){
  u_int32_t n, sz = 0;
  u_int8_t c;
  if( dbsql_value_type(arg)!=DBSQL_BLOB ) return 0;
  p->blobs = (u_int8_t*)dbsql_value_blob(arg);
  p->n_blob = (u_int32_t)dbsql_value_bytes(arg);
  if( p->n_blob>0
   && ALWAYS(p->blobs!=0)
   && ((c = p->blobs[0]) & 0x0f)<=JSONB_OBJECT
   && (n = __jsonb_payload_size(p, 0, &sz))>0
   && sz+n==p->n_blob
   && ((c & 0x0f)>JSONB_FALSE || sz==0)
   && (sz>7 
      || (c!=0x7b && c!=0x5b && !__isdigit(c))
      || __jsonb_validity_check(p, 0, p->n_blob, 1)==0)
  ){
    return 1;
  }
  p->blobs = 0;
  p->n_blob = 0;
  return 0;
}

/*
** Generate a json_parse_t object, containing valid JSONB in blobs and n_blob,
** from the SQL function argument arg.  Return a pointer to the new
** json_parse_t object.
**
** Ownership of the new json_parse_t object is passed to the caller.  The
** caller should invoke __json_parse_free() on the return value when it
** has finished using it.
**
** If any errors are detected, an appropriate error messages is set
** using dbsql_result_error() or the equivalent and this routine
** returns NULL.  This routine also returns NULL if the arg argument
** is an SQL NULL value, but no error message is set in that case.  This
** is so that SQL functions that are given NULL arguments will return
** a NULL value.
*/
static json_parse_t *__json_parse_func_arg(
  dbsql_func_t *ctx,
  dbsql_value_t *arg,
  u_int32_t flgs
){
  int type;                   /* Datatype of arg */
  json_parse_t *p = 0;            /* Value to be returned */
  json_parse_t *from_cache = 0;   /* Value taken from cache */
  DBSQL *db;                 /* The database connection */
  
  assert( ctx!=0 );
  type = dbsql_value_type(arg);
  if( type==DBSQL_NULL ){
    return 0;
  }
  from_cache = __json_cache_search(ctx, arg);
  if( from_cache ){
    from_cache->n_jp_ref++;
    if( (flgs & JSON_EDITABLE)==0 ){
      return from_cache;
    }
  }
  db = dbsql_context_db_handle(ctx);
rebuild_from_cache:
  p = __db_malloc_zero(db, sizeof(*p));
  if( p==0 ) goto json_pfa_oom;
  memset(p, 0, sizeof(*p));
  p->db = db;
  p->n_jp_ref = 1;
  if( from_cache!=0 ){
    u_int32_t n_blob = from_cache->n_blob;
    p->blobs = __db_malloc_raw(db, n_blob);
    if( p->blobs==0 ) goto json_pfa_oom;
    memcpy(p->blobs, from_cache->blobs, n_blob);
    p->n_blob_alloc = p->n_blob = n_blob;
    p->has_nonstd = from_cache->has_nonstd;
    __json_parse_free(from_cache);
    return p;
  }
  if( type==DBSQL_BLOB ){
    if( __json_arg_is_jsonb(arg,p) ){
      if( (flgs & JSON_EDITABLE)!=0 && __json_blob_make_editable(p, 0)==0 ){
        goto json_pfa_oom;
      }
      return p;
    }
    /* If the blob is not valid JSONB, fall through into trying to cast
    ** the blob into text which is then interpreted as JSON.  (tag-20240123-a)
    **
    ** This goes against all historical documentation about how the dbsql
    ** JSON functions were suppose to work.  From the beginning, blob was
    ** reserved for expansion and a blob value should have raised an error.
    ** But it did not, due to a bug.  And many applications came to depend
    ** upon this buggy behavior, especially when using the CLI and reading
    ** JSON text using readfile(), which returns a blob.  For this reason
    ** we will continue to support the bug moving forward.
    ** See for example https://sqlite.org/forum/forumpost/012136abd5292b8d
    */
  }
  p->json = (char*)dbsql_value_text(arg);
  p->n_json = dbsql_value_bytes(arg);
  if( db->malloc_failed ) goto json_pfa_oom;
  if( p->n_json==0 ) goto json_pfa_malformed;
  assert( p->json!=0 );
  if( __json_convert_text_to_blob(p, (flgs & JSON_KEEPERROR) ? 0 : ctx) ){
    if( flgs & JSON_KEEPERROR ){
      p->n_err = 1;
      return p;
    }else{
      __json_parse_free(p);
      return 0;
    }
  }else{
    int is_rc_str = __value_is_of_class(arg, __rc_str_unref);
    int rc;
    if( !is_rc_str ){
      char *z_new = __rc_str_new( p->n_json );
      if( z_new==0 ) goto json_pfa_oom;
      memcpy(z_new, p->json, p->n_json);
      p->json = z_new;
      p->json[p->n_json] = 0;
    }else{
      __rc_str_ref(p->json);
    }
    p->json_is_rc_str = 1;
    rc = __json_cache_insert(ctx, p);
    if( rc==DBSQL_NOMEM ) goto json_pfa_oom;
    if( flgs & JSON_EDITABLE ){
      from_cache = p;
      p = 0;
      goto rebuild_from_cache;
    }
  }
  return p;

json_pfa_malformed:
  if( flgs & JSON_KEEPERROR ){
    p->n_err = 1;
    return p;
  }else{
    __json_parse_free(p);
    dbsql_result_error(ctx, "malformed JSON", -1);
    return 0;
  }

json_pfa_oom:
  __json_parse_free(from_cache);
  __json_parse_free(p);
  dbsql_result_error_nomem(ctx);
  return 0;
}

/*
** Make the return value of a JSON function either the raw JSONB blob
** or make it JSON text, depending on whether the JSON_BLOB flag is
** set on the function.
*/
static void __json_return_parse(
  dbsql_func_t *ctx,
  json_parse_t *p
){
  int flgs;
  if( p->oom ){
    dbsql_result_error_nomem(ctx);
    return;
  }
  flgs = DBSQL_PTR_TO_INT(dbsql_user_data(ctx));
  if( flgs & JSON_BLOB ){
    if( p->n_blob_alloc>0 && !p->read_only ){
      dbsql_result_blob(ctx, p->blobs, p->n_blob, DBSQL_DYNAMIC);
      p->n_blob_alloc = 0;
    }else{
      dbsql_result_blob(ctx, p->blobs, p->n_blob, DBSQL_TRANSIENT);
    }
  }else{
    json_string_t s;
    __json_string_init(&s, ctx);
    p->delta = 0;
    __json_translate_blob_to_text(p, 0, &s);
    __json_return_string(&s, p, ctx);
    dbsql_result_subtype(ctx, JSON_SUBTYPE);
  }
}

/****************************************************************************
** SQL functions used for testing and debugging
****************************************************************************/

#if DBSQL_DEBUG
/*
** Decode JSONB bytes in blobs[] starting at i_start through but not 
** including i_end.  Indent the
** content by n_indent spaces.
*/
static void __json_debug_print_blob(
  json_parse_t *parser, /* JSON content */
  u_int32_t i_start,        /* Start rendering here */
  u_int32_t i_end,          /* Do not render this byte or any byte after this one */
  int n_indent,       /* Indent by this many spaces */
  dbsql_str_t *out  /* Generate output into this dbsql_str_t object */
){
  while( i_start<i_end ){
    u_int32_t i, n, nn, sz = 0;
    int show_content = 1;
    u_int8_t x = parser->blobs[i_start] & 0x0f;
    u_int32_t saved_n_blob = parser->n_blob;
    dbsql_str_appendf(out, "%5d:%*s", i_start, n_indent, "");
    if( parser->n_blob_alloc>parser->n_blob ){
      parser->n_blob = parser->n_blob_alloc;
    }
    nn = n = __jsonb_payload_size(parser, i_start, &sz);
    if( nn==0 ) nn = 1;
    if( sz>0 && x<JSONB_ARRAY ){
      nn += sz;
    }
    for(i=0; i<nn; i++){
      dbsql_str_appendf(out, " %02x", parser->blobs[i_start+i]);
    }
    if( n==0 ){
      dbsql_str_appendf(out, "   ERROR invalid node size\n");
      i_start = n==0 ? i_start+1 : i_end;
      continue;
    }
    parser->n_blob = saved_n_blob;
    if( i_start+n+sz>i_end ){
      i_end = i_start+n+sz;
      if( i_end>parser->n_blob ){
        if( parser->n_blob_alloc>0 && i_end>parser->n_blob_alloc ){
          i_end = parser->n_blob_alloc;
        }else{
          i_end = parser->n_blob;
        }
      }
    }
    dbsql_str_appendall(out,"  <-- ");
    switch( x ){
      case JSONB_NULL:     dbsql_str_appendall(out,"null"); break;
      case JSONB_TRUE:     dbsql_str_appendall(out,"true"); break;
      case JSONB_FALSE:    dbsql_str_appendall(out,"false"); break;
      case JSONB_INT:      dbsql_str_appendall(out,"int"); break;
      case JSONB_INT5:     dbsql_str_appendall(out,"int5"); break;
      case JSONB_FLOAT:    dbsql_str_appendall(out,"float"); break;
      case JSONB_FLOAT5:   dbsql_str_appendall(out,"float5"); break;
      case JSONB_TEXT:     dbsql_str_appendall(out,"text"); break;
      case JSONB_TEXTJ:    dbsql_str_appendall(out,"textj"); break;
      case JSONB_TEXT5:    dbsql_str_appendall(out,"text5"); break;
      case JSONB_TEXTRAW:  dbsql_str_appendall(out,"textraw"); break;
      case JSONB_ARRAY: {
        dbsql_str_appendf(out,"array, %u bytes\n", sz);
        __json_debug_print_blob(parser, i_start+n, i_start+n+sz, n_indent+2, out);
        show_content = 0;
        break;
      }
      case JSONB_OBJECT: {
        dbsql_str_appendf(out, "object, %u bytes\n", sz);
        __json_debug_print_blob(parser, i_start+n, i_start+n+sz, n_indent+2, out);
        show_content = 0;
        break;
      }
      default: {
        dbsql_str_appendall(out, "ERROR: unknown node type\n");
        show_content = 0;
        break;
      }
    }
    if( show_content ){
      if( sz==0 && x<=JSONB_FALSE ){
        dbsql_str_append(out, "\n", 1);
      }else{
        u_int32_t j;
        dbsql_str_appendall(out, ": \"");
        for(j=i_start+n; j<i_start+n+sz; j++){
          u_int8_t c = parser->blobs[j];
          if( c<0x20 || c>=0x7f ) c = '.';
          dbsql_str_append(out, (char*)&c, 1);
        }
        dbsql_str_append(out, "\"\n", 2);
      }
    }
    i_start += n + sz;
  }
}
static void __json_show_parse(json_parse_t *parser){
  dbsql_str_t out;
  char buf[1000];
  if( parser==0 ){
    printf("NULL pointer\n");
    return;
  }else{
    printf("nBlobAlloc = %u\n", parser->n_blob_alloc);
    printf("nBlob = %u\n", parser->n_blob);
    printf("delta = %d\n", parser->delta);
    if( parser->n_blob==0 ) return;
    printf("content (bytes 0..%u):\n", parser->n_blob-1);
  }
  __str_accum_init(&out, 0, buf, sizeof(buf), 1000000);
  __json_debug_print_blob(parser, 0, parser->n_blob, 0, &out);
  printf("%s", dbsql_str_value(&out));
  dbsql_str_reset(&out);
}
#endif /* DBSQL_DEBUG */

#ifdef DBSQL_DEBUG
/*
** SQL function:   json_parse(JSON)
**
** parser_t JSON using __json_parse_func_arg().  Return text that is a
** human-readable dump of the binary JSONB for the input parameter.
*/
static void __json_parse_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p;        /* The parse */
  dbsql_str_t out;

  assert( argc>=1 );
  __str_accum_init(&out, 0, 0, 0, 1000000);
  p = __json_parse_func_arg(ctx, argv[0], 0);
  if( p==0 ) return;
  if( argc==1 ){
    __json_debug_print_blob(p, 0, p->n_blob, 0, &out);
    dbsql_result_text64(ctx,out.text,out.n_char,DBSQL_TRANSIENT,DBSQL_UTF8);
  }else{
    __json_show_parse(p);
  }
  __json_parse_free(p);
  dbsql_str_reset(&out);
}
#endif /* DBSQL_DEBUG */

/****************************************************************************
** Scalar SQL function implementations
****************************************************************************/

/*
** Implementation of the json_quote(VALUE) function.  Return a JSON value
** corresponding to the SQL value input.  Mostly this means putting
** double-quotes around strings and returning the unquoted string "null"
** when given a NULL input.
*/
static void __json_quote_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_string_t jx;
  UNUSED_PARAMETER(argc);

  __json_string_init(&jx, ctx);
  __json_append_sql_value(&jx, argv[0]);
  __json_return_string(&jx, 0, 0);
  dbsql_result_subtype(ctx, JSON_SUBTYPE);
}

/*
** Implementation of the json_array(VALUE,...) function.  Return a JSON
** array that contains all values given in arguments.  Or if any argument
** is a BLOB, throw an error.
*/
static void __json_array_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  int i;
  json_string_t jx;

  __json_string_init(&jx, ctx);
  __json_append_char(&jx, '[');
  for(i=0; i<argc; i++){
    __json_append_separator(&jx);
    __json_append_sql_value(&jx, argv[i]);
  }
  __json_append_char(&jx, ']');
  __json_return_string(&jx, 0, 0);
  dbsql_result_subtype(ctx, JSON_SUBTYPE);
}

/*
** json_array_length(JSON)
** json_array_length(JSON, PATH)
**
** Return the number of elements in the top-level JSON array.
** Return 0 if the input is not a well-formed JSON array.
*/
static void __json_array_length_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p;          /* The parse */
  int64_t cnt = 0;
  u_int32_t i;
  u_int8_t err = 0;

  p = __json_parse_func_arg(ctx, argv[0], 0);
  if( p==0 ) return;
  if( argc==2 ){
    const char *path = (const char*)dbsql_value_text(argv[1]);
    if( path==0 ){
      __json_parse_free(p);
      return;
    }
    i = __json_lookup_step(p, 0, path[0]=='$' ? path+1 : "@", 0);
    if( JSON_LOOKUP_ISERROR(i) ){
      if( i==JSON_LOOKUP_NOTFOUND ){
        /* no-op */
      }else{
        __json_bad_path_error(ctx, path, i);
      }
      err = 1;
      i = 0;
    }
  }else{
    i = 0;
  }
  if( (p->blobs[i] & 0x0f)==JSONB_ARRAY ){
    cnt = __jsonb_array_count(p, i);
  }
  if( !err ) dbsql_result_int64(ctx, cnt);
  __json_parse_free(p);
}

/* True if the string is all alphanumerics and underscores */
static int __json_all_alphanum(const char *z, int n){
  int i;
  for(i=0; i<n && (__isalnum(z[i]) || z[i]=='_'); i++){}
  return i==n;
}

/*
** json_extract(JSON, PATH, ...)
** "->"(JSON,PATH)
** "->>"(JSON,PATH)
**
** Return the element described by PATH.  Return NULL if that PATH element
** is not found.
**
** If JSON_JSON is set or if more that one PATH argument is supplied then
** always return a JSON representation of the result.  If JSON_SQL is set,
** then always return an SQL representation of the result.  If neither flag
** is present and argc==2, then return JSON for objects and arrays and SQL
** for all other values.
**
** When multiple PATH arguments are supplied, the result is a JSON array
** containing the result of each PATH.
**
** Abbreviated JSON path expressions are allows if JSON_ABPATH, for
** compatibility with PG.
*/
static void __json_extract_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p = 0;      /* The parse */
  int flags;             /* Flags associated with the function */
  int i;                 /* Loop counter */
  json_string_t jx;         /* String for array result */

  if( argc<2 ) return;
  p = __json_parse_func_arg(ctx, argv[0], 0);
  if( p==0 ) return;
  flags = DBSQL_PTR_TO_INT(dbsql_user_data(ctx));
  __json_string_init(&jx, ctx);
  if( argc>2 ){
    __json_append_char(&jx, '[');
  }
  for(i=1; i<argc; i++){
    /* with_t a single PATH argument */
    const char *path = (const char*)dbsql_value_text(argv[i]);
    int n_path;
    u_int32_t j;
    if( path==0 ) goto json_extract_error;
    n_path = __strlen30(path);
    if( path[0]=='$' ){
      j = __json_lookup_step(p, 0, path+1, 0);
    }else if( (flags & JSON_ABPATH) ){
      /* The -> and ->> operators accept abbreviated PATH arguments.  This
      ** is mostly for compatibility with __postgre_sql, but also for
      ** convenience.
      **
      **     NUMBER   ==>  $[NUMBER]     // PG compatible
      **     LABEL    ==>  $.LABEL       // PG compatible
      **     [NUMBER] ==>  $[NUMBER]     // Not PG.  Purely for convenience
      **
      ** Updated 2024-05-27:  If the NUMBER is negative, then PG counts from
      ** the right of the array.  Hence for negative NUMBER:
      **
      **     NUMBER   ==>  $[#NUMBER]    // PG compatible
      */
      __json_string_init(&jx, ctx);
      if( dbsql_value_type(argv[i])==DBSQL_INTEGER ){
        __json_append_raw_nz(&jx, "[", 1);
        if( path[0]=='-' ) __json_append_raw_nz(&jx,"#",1);
        __json_append_raw(&jx, path, n_path);
        __json_append_raw_nz(&jx, "]", 2);
      }else if( __json_all_alphanum(path, n_path) ){
        __json_append_raw_nz(&jx, ".", 1);
        __json_append_raw(&jx, path, n_path);
      }else if( path[0]=='[' && n_path>=3 && path[n_path-1]==']' ){
        __json_append_raw(&jx, path, n_path);
      }else{
        __json_append_raw_nz(&jx, ".\"", 2);
        __json_append_raw(&jx, path, n_path);
        __json_append_raw_nz(&jx, "\"", 1);
      }
      __json_string_terminate(&jx);
      j = __json_lookup_step(p, 0, jx.buf, 0);
      __json_string_reset(&jx);
    }else{
      __json_bad_path_error(ctx, path, 0);
      goto json_extract_error;
    }
    if( j<p->n_blob ){
      if( argc==2 ){
        if( flags & JSON_JSON ){
          __json_string_init(&jx, ctx);
          __json_translate_blob_to_text(p, j, &jx);
          __json_return_string(&jx, 0, 0);
          __json_string_reset(&jx);
          assert( (flags & JSON_BLOB)==0 );
          dbsql_result_subtype(ctx, JSON_SUBTYPE);
        }else{
          __json_return_from_blob(p, j, ctx, 0);
          if( (flags & (JSON_SQL|JSON_BLOB))==0
           && (p->blobs[j]&0x0f)>=JSONB_ARRAY
          ){
            dbsql_result_subtype(ctx, JSON_SUBTYPE);
          }
        }
      }else{
        __json_append_separator(&jx);
        __json_translate_blob_to_text(p, j, &jx);
      }
    }else if( j==JSON_LOOKUP_NOTFOUND ){
      if( argc==2 ){
        goto json_extract_error;  /* Return NULL if not found */
      }else{
        __json_append_separator(&jx);
        __json_append_raw_nz(&jx, "null", 4);
      }
    }else{
      __json_bad_path_error(ctx, path, j);
      goto json_extract_error;
    }
  }
  if( argc>2 ){
    __json_append_char(&jx, ']');
    __json_return_string(&jx, 0, 0);
    if( (flags & JSON_BLOB)==0 ){
      dbsql_result_subtype(ctx, JSON_SUBTYPE);
    }
  }
json_extract_error:
  __json_string_reset(&jx);
  __json_parse_free(p);
  return;
}

/*
** Return codes for __json_merge_patch()
*/
#define JSON_MERGE_OK          0     /* Success */
#define JSON_MERGE_BADTARGET   1     /* Malformed TARGET blob */
#define JSON_MERGE_BADPATCH    2     /* Malformed PATCH blob */
#define JSON_MERGE_OOM         3     /* Out-of-memory condition */
#define JSON_MERGE_TOODEEP     4     /* Nested too deep */

/*
** RFC-7396 __merge_patch for two JSONB blobs.
**
** target is the target. patch is the patch.  The target is updated
** in place.  The patch is read-only.
**
** The original RFC-7396 algorithm is this:
**
**   define __merge_patch(Target, Patch):
**     if Patch is an Object:
**       if Target is not an Object:
**         Target = {} # Ignore the contents and set it to an empty Object
**     for each Name/Value pair in Patch:
**         if Value is null:
**           if Name exists in Target:
**             remove the Name/Value pair from Target
**         else:
**           Target[Name] = __merge_patch(Target[Name], Value)
**       return Target
**     else:
**       return Patch
**
** Here is an equivalent algorithm restructured to show the actual
** implementation:
**
** 01   define __merge_patch(Target, Patch):
** 02      if Patch is not an Object:
** 03         return Patch
** 04      else: // if Patch is an Object
** 05         if Target is not an Object:
** 06            Target = {}
** 07      for each Name/Value pair in Patch:
** 08         if Name exists in Target:
** 09            if Value is null:
** 10               remove the Name/Value pair from Target
** 11            else
** 12               Target[name] = __merge_patch(Target[Name], Value)
** 13         else if Value is not NULL:
** 14            if Value is not an Object:
** 15               Target[name] = Value
** 16            else:
** 17               Target[name] = __merge_patch('{}',value)
** 18      return Target
**  |
**  ^---- Line numbers referenced in comments in the implementation
*/
static int __json_merge_patch(
  json_parse_t *target,      /* The JSON parser that contains the TARGET */
  u_int32_t i_target,             /* index_t of TARGET in target->blobs[] */
  const json_parse_t *patch, /* The PATCH */
  u_int32_t i_patch,              /* index_t of PATCH in patch->blobs[] */
  u_int32_t i_depth               /* Nesting depth */
){
  u_int8_t x;             /* Type of a single node */
  u_int32_t n, sz=0;      /* Return values from __jsonb_payload_size() */
  u_int32_t i_t_cursor;     /* Cursor position while scanning the target object */
  u_int32_t i_t_start;      /* First label in the target object */
  u_int32_t i_t_end_be;      /* Original first byte past end of target, before edit */
  u_int32_t i_t_end;        /* Current first byte past end of target */
  u_int8_t t_label;       /* Node type of the target label */
  u_int32_t i_t_label = 0;  /* index_t of the label */
  u_int32_t n_t_label = 0;  /* Header size in bytes for the target label */
  u_int32_t sz_t_label = 0; /* Size of the target label payload */
  u_int32_t i_t_value = 0;  /* index_t of the target value */
  u_int32_t n_t_value = 0;  /* Header size of the target value */
  u_int32_t sz_t_value = 0; /* Payload size for the target value */

  u_int32_t i_p_cursor;     /* Cursor position while scanning the patch */
  u_int32_t i_p_end;        /* First byte past the end of the patch */
  u_int8_t p_label;       /* Node type of the patch label */
  u_int32_t i_p_label;      /* Start of patch label */
  u_int32_t n_p_label;      /* Size of header on the patch label */
  u_int32_t sz_p_label;     /* Payload size of the patch label */
  u_int32_t i_p_value;      /* Start of patch value */
  u_int32_t n_p_value;      /* Header size for the patch value */
  u_int32_t sz_p_value;     /* Payload size of the patch value */

  assert( i_target>=0 && i_target<target->n_blob );
  assert( i_patch>=0 && i_patch<patch->n_blob );
  x = patch->blobs[i_patch] & 0x0f;
  if( x!=JSONB_OBJECT ){  /* Algorithm line 02 */
    u_int32_t sz_patch;        /* Total size of the patch, header+payload */
    u_int32_t sz_target;       /* Total size of the target, header+payload */
    n = __jsonb_payload_size(patch, i_patch, &sz);
    sz_patch = n+sz;
    sz = 0;
    n = __jsonb_payload_size(target, i_target, &sz);
    sz_target = n+sz;
    __json_blob_edit(target, i_target, sz_target, patch->blobs+i_patch, sz_patch);
    return target->oom ? JSON_MERGE_OOM : JSON_MERGE_OK;  /* Line 03 */
  }
  x = target->blobs[i_target] & 0x0f;
  if( x!=JSONB_OBJECT ){  /* Algorithm line 05 */
    n = __jsonb_payload_size(target, i_target, &sz);
    __json_blob_edit(target, i_target+n, sz, 0, 0);
    x = target->blobs[i_target];
    target->blobs[i_target] = (x & 0xf0) | JSONB_OBJECT;
  }
  n = __jsonb_payload_size(patch, i_patch, &sz);
  if( NEVER(n==0) ) return JSON_MERGE_BADPATCH;
  i_p_cursor = i_patch+n;
  i_p_end = i_p_cursor+sz;
  n = __jsonb_payload_size(target, i_target, &sz);
  if( NEVER(n==0) ) return JSON_MERGE_BADTARGET;
  i_t_start = i_target+n;
  i_t_end_be = i_t_start+sz;

  while( i_p_cursor<i_p_end ){  /* Algorithm line 07 */
    i_p_label = i_p_cursor;
    p_label = patch->blobs[i_p_cursor] & 0x0f;
    if( p_label<JSONB_TEXT || p_label>JSONB_TEXTRAW ){
      return JSON_MERGE_BADPATCH;
    }
    n_p_label = __jsonb_payload_size(patch, i_p_cursor, &sz_p_label);
    if( n_p_label==0 ) return JSON_MERGE_BADPATCH;
    i_p_value = i_p_cursor + n_p_label + sz_p_label;
    if( i_p_value>=i_p_end ) return JSON_MERGE_BADPATCH;
    n_p_value = __jsonb_payload_size(patch, i_p_value, &sz_p_value);
    if( n_p_value==0 ) return JSON_MERGE_BADPATCH;
    i_p_cursor = i_p_value + n_p_value + sz_p_value;
    if( i_p_cursor>i_p_end ) return JSON_MERGE_BADPATCH;

    i_t_cursor = i_t_start;
    i_t_end = i_t_end_be + target->delta;
    while( i_t_cursor<i_t_end ){
      int is_equal;   /* true if the patch and target labels match */
      i_t_label = i_t_cursor;
      t_label = target->blobs[i_t_cursor] & 0x0f;
      if( t_label<JSONB_TEXT || t_label>JSONB_TEXTRAW ){
        return JSON_MERGE_BADTARGET;
      }
      n_t_label = __jsonb_payload_size(target, i_t_cursor, &sz_t_label);
      if( n_t_label==0 ) return JSON_MERGE_BADTARGET;
      i_t_value = i_t_label + n_t_label + sz_t_label;
      if( i_t_value>=i_t_end ) return JSON_MERGE_BADTARGET;
      n_t_value = __jsonb_payload_size(target, i_t_value, &sz_t_value);
      if( n_t_value==0 ) return JSON_MERGE_BADTARGET;
      if( i_t_value + n_t_value + sz_t_value > i_t_end ) return JSON_MERGE_BADTARGET;
      is_equal = __json_label_compare(
                   (const char*)&patch->blobs[i_p_label+n_p_label],
                   sz_p_label,
                   (p_label==JSONB_TEXT || p_label==JSONB_TEXTRAW),
                   (const char*)&target->blobs[i_t_label+n_t_label],
                   sz_t_label,
                   (t_label==JSONB_TEXT || t_label==JSONB_TEXTRAW));
      if( is_equal ) break;
      i_t_cursor = i_t_value + n_t_value + sz_t_value;
    }
    x = patch->blobs[i_p_value] & 0x0f;
    if( i_t_cursor<i_t_end ){
      /* A match was found.  Algorithm line 08 */
      if( x==0 ){
        /* Patch value is NULL.  Algorithm line 09 */
        __json_blob_edit(target, i_t_label, n_t_label+sz_t_label+n_t_value+sz_t_value, 0,0);
        /*  vvvvvv----- No OOM on a delete-only edit */
        if( NEVER(target->oom) ) return JSON_MERGE_OOM;
      }else{
        /* Algorithm line 12 */
        int rc, saved_delta = target->delta;
        target->delta = 0;
        if( i_depth>=JSON_MAX_DEPTH ) return JSON_MERGE_TOODEEP;
        rc = __json_merge_patch(target, i_t_value, patch, i_p_value, i_depth+1);
        if( rc ) return rc;
        target->delta += saved_delta;
      }        
    }else if( x>0 ){  /* Algorithm line 13 */
      /* No match and patch value is not NULL */
      u_int32_t sz_new = sz_p_label+n_p_label;
      if( (patch->blobs[i_p_value] & 0x0f)!=JSONB_OBJECT ){  /* Line 14 */
        __json_blob_edit(target, i_t_end, 0, 0, sz_p_value+n_p_value+sz_new);
        if( target->oom ) return JSON_MERGE_OOM;
        memcpy(&target->blobs[i_t_end], &patch->blobs[i_p_label], sz_new);
        memcpy(&target->blobs[i_t_end+sz_new], 
               &patch->blobs[i_p_value], sz_p_value+n_p_value);
      }else{
        int rc, saved_delta;
        __json_blob_edit(target, i_t_end, 0, 0, sz_new+1);
        if( target->oom ) return JSON_MERGE_OOM;
        memcpy(&target->blobs[i_t_end], &patch->blobs[i_p_label], sz_new);
        target->blobs[i_t_end+sz_new] = 0x00;
        saved_delta = target->delta;
        target->delta = 0;
        if( i_depth>=JSON_MAX_DEPTH ) return JSON_MERGE_TOODEEP;
        rc = __json_merge_patch(target, i_t_end+sz_new,patch,i_p_value,i_depth+1);
        if( rc ) return rc;
        target->delta += saved_delta;
      }
    }
  }
  if( target->delta ) __json_after_edit_size_adjust(target, i_target);
  return target->oom ? JSON_MERGE_OOM : JSON_MERGE_OK;
}


/*
** Implementation of the json_mergepatch(JSON1,JSON2) function.  Return a JSON
** object that is the result of running the RFC 7396 __merge_patch() algorithm
** on the two arguments.
*/
static void __json_patch_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *target;    /* The TARGET */
  json_parse_t *patch;     /* The PATCH */
  int rc;                /* Result code */

  UNUSED_PARAMETER(argc);
  assert( argc==2 );
  target = __json_parse_func_arg(ctx, argv[0], JSON_EDITABLE);
  if( target==0 ) return;
  patch = __json_parse_func_arg(ctx, argv[1], 0);
  if( patch ){
    rc = __json_merge_patch(target, 0, patch, 0, 0);
    if( rc==JSON_MERGE_OK ){
      __json_return_parse(ctx, target);
    }else if( rc==JSON_MERGE_OOM ){
      dbsql_result_error_nomem(ctx);
    }else if( rc==JSON_MERGE_TOODEEP ){
      dbsql_result_error(ctx, "JSON nested too deep", -1);
    }else{
      dbsql_result_error(ctx, "malformed JSON", -1);
    }
    __json_parse_free(patch);
  }
  __json_parse_free(target);
}


/*
** Implementation of the json_object(NAME,VALUE,...) function.  Return a JSON
** object that contains all name/value given in arguments.  Or if any name
** is not a string or if any value is a BLOB, throw an error.
*/
static void __json_object_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  int i;
  json_string_t jx;
  const char *z;
  u_int32_t n;

  if( argc&1 ){
    dbsql_result_error(ctx, "json_object() requires an even number "
                                  "of arguments", -1);
    return;
  }
  __json_string_init(&jx, ctx);
  __json_append_char(&jx, '{');
  for(i=0; i<argc; i+=2){
    if( dbsql_value_type(argv[i])!=DBSQL_TEXT ){
      dbsql_result_error(ctx, "json_object() labels must be TEXT", -1);
      __json_string_reset(&jx);
      return;
    }
    __json_append_separator(&jx);
    z = (const char*)dbsql_value_text(argv[i]);
    n = dbsql_value_bytes(argv[i]);
    __json_append_string(&jx, z, n);
    __json_append_char(&jx, ':');
    __json_append_sql_value(&jx, argv[i+1]);
  }
  __json_append_char(&jx, '}');
  __json_return_string(&jx, 0, 0);
  dbsql_result_subtype(ctx, JSON_SUBTYPE);
}


/*
** json_remove(JSON, PATH, ...)
**
** Remove the named elements from JSON and return the result.  malformed
** JSON or PATH arguments result in an error.
*/
static void __json_remove_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p;          /* The parse */
  const char *path = 0; /* Path of element to be removed */
  int i;                 /* Loop counter */
  u_int32_t rc;                /* Subroutine return code */

  if( argc<1 ) return;
  p = __json_parse_func_arg(ctx, argv[0], argc>1 ? JSON_EDITABLE : 0);
  if( p==0 ) return;
  for(i=1; i<argc; i++){
    path = (const char*)dbsql_value_text(argv[i]);
    if( path==0 ){
      goto json_remove_done;
    }
    if( path[0]!='$' ){
      goto json_remove_patherror;
    }
    if( path[1]==0 ){
      /* json_remove(j,'$') returns NULL */
      goto json_remove_done;
    }
    p->edit = JEDIT_DEL;
    p->delta = 0;
    rc = __json_lookup_step(p, 0, path+1, 0);
    if( JSON_LOOKUP_ISERROR(rc) ){
      if( rc==JSON_LOOKUP_NOTFOUND ){
        continue;  /* No-op */
      }else{
        __json_bad_path_error(ctx, path, rc);
      }
      goto json_remove_done;
    }
  }
  __json_return_parse(ctx, p);
  __json_parse_free(p);
  return;

json_remove_patherror:
  __json_bad_path_error(ctx, path, 0);

json_remove_done:
  __json_parse_free(p);
  return;
}

/*
** json_replace(JSON, PATH, VALUE, ...)
**
** Replace the value at PATH with VALUE.  If PATH does not already exist,
** this routine is a no-op.  If JSON or PATH is malformed, throw an error.
*/
static void __json_replace_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  if( argc<1 ) return;
  if( (argc&1)==0 ) {
    __json_wrong_num_args(ctx, "replace");
    return;
  }
  __json_insert_into_blob(ctx, argc, argv, JEDIT_REPL);
}


/*
** json_set(JSON, PATH, VALUE, ...)
**
** Set the value at PATH to VALUE.  Create the PATH if it does not already
** exist.  Overwrite existing values that do exist.
** If JSON or PATH is malformed, throw an error.
**
** json_insert(JSON, PATH, VALUE, ...)
**
** Create PATH and initialize it to VALUE.  If PATH already exists, this
** routine is a no-op.  If JSON or PATH is malformed, throw an error.
*/
static void __json_set_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  int flags = DBSQL_PTR_TO_INT(dbsql_user_data(ctx));
  int ins_type = JSON_INSERT_TYPE(flags);
  static const char *ins_types[] = { "insert", "set", "array_insert" };
  static const u_int8_t edit_types[] = { JEDIT_INS, JEDIT_SET, JEDIT_AINS };

  if( argc<1 ) return;
  assert( ins_type>=0 && ins_type<=2 );
  if( (argc&1)==0 ) {
    __json_wrong_num_args(ctx, ins_types[ins_type]);
    return;
  }
  __json_insert_into_blob(ctx, argc, argv, edit_types[ins_type]);
}

/*
** json_type(JSON)
** json_type(JSON, PATH)
**
** Return the top-level "type" of a JSON string.  json_type() raises an
** error if either the JSON or PATH inputs are not well-formed.
*/
static void __json_type_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p;          /* The parse */
  const char *path = 0;
  u_int32_t i;

  p = __json_parse_func_arg(ctx, argv[0], 0);
  if( p==0 ) return;
  if( argc==2 ){
    path = (const char*)dbsql_value_text(argv[1]);
    if( path==0 ) goto json_type_done;
    if( path[0]!='$' ){
      __json_bad_path_error(ctx, path, 0);
      goto json_type_done;
    }
    i = __json_lookup_step(p, 0, path+1, 0);
    if( JSON_LOOKUP_ISERROR(i) ){
      if( i==JSON_LOOKUP_NOTFOUND ){
        /* no-op */
      }else{
        __json_bad_path_error(ctx, path, i);
      }
      goto json_type_done;
    }
  }else{
    i = 0;
  }
  dbsql_result_text(ctx, __jsonb_type[p->blobs[i]&0x0f], -1, DBSQL_STATIC);
json_type_done:
  __json_parse_free(p);
}

/*
** json_pretty(JSON)
** json_pretty(JSON, INDENT)
**
** Return text that is a pretty-printed rendering of the input JSON.
** If the argument is not valid JSON, return NULL.
**
** The INDENT argument is text that is used for indentation.  If omitted,
** it defaults to four spaces (the same as __postgre_sql).
*/
static void __json_pretty_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_string_t s;          /* The output string */
  json_pretty_t x;          /* Pretty printing context */

  memset(&x, 0, sizeof(x));
  x.parser = __json_parse_func_arg(ctx, argv[0], 0);
  if( x.parser==0 ) return;
  x.out = &s;
  __json_string_init(&s, ctx);
  if( argc==1 || (x.indent = (const char*)dbsql_value_text(argv[1]))==0 ){
    x.indent = "    ";
    x.sz_indent = 4;
  }else{
    x.sz_indent = (u_int32_t)strlen(x.indent);
  }
  __json_translate_blob_to_pretty_text(&x, 0);
  __json_return_string(&s, 0, 0);
  __json_parse_free(x.parser);
}

/*
** json_valid(JSON)
** json_valid(JSON, FLAGS)
**
** Check the JSON argument to see if it is well-formed.  The FLAGS argument
** encodes the various constraints on what is meant by "well-formed":
**
**     0x01      Canonical RFC-8259 JSON text
**     0x02      JSON text with optional JSON-5 extensions
**     0x04      Superficially appears to be JSONB
**     0x08      Strictly well-formed JSONB
**
** If the FLAGS argument is omitted, it defaults to 1.  Useful values for
** FLAGS include:
**
**    1          Strict canonical JSON text
**    2          JSON text perhaps with JSON-5 extensions
**    4          Superficially appears to be JSONB
**    5          Canonical JSON text or superficial JSONB
**    6          JSON-5 text or superficial JSONB
**    8          Strict JSONB
**    9          Canonical JSON text or strict JSONB
**    10         JSON-5 text or strict JSONB
**
** Other flag combinations are redundant.  For example, every canonical
** JSON text is also well-formed JSON-5 text, so FLAG values 2 and 3
** are the same.  Similarly, any input that passes a strict JSONB validation
** will also pass the superficial validation so 12 through 15 are the same
** as 8 through 11 respectively.
**
** This routine runs in linear time to validate text and when doing strict
** JSONB validation.  Superficial JSONB validation is constant time,
** assuming the BLOB is already in memory.  The performance advantage
** of superficial JSONB validation is why that option is provided.
** Application developers can choose to do fast superficial validation or
** slower strict validation, according to their specific needs.
**
** Only the lower four bits of the FLAGS argument are currently used.
** Higher bits are reserved for future expansion.   To facilitate
** compatibility, the current implementation raises an error if any bit
** in FLAGS is set other than the lower four bits.
**
** The original circa 2015 implementation of the JSON routines in
** dbsql only supported canonical RFC-8259 JSON text and the json_valid()
** function only accepted one argument.  That is why the default value
** for the FLAGS argument is 1, since FLAGS=1 causes this routine to only
** recognize canonical RFC-8259 JSON text as valid.  The extra FLAGS
** argument was added when the JSON routines were extended to support
** JSON5-like extensions and binary JSONB stored in BLOBs.
**
** Return Values:
**
**   *   Raise an error if FLAGS is outside the range of 1 to 15.
**   *   Return NULL if the input is NULL
**   *   Return 1 if the input is well-formed.
**   *   Return 0 if the input is not well-formed.
*/
static void __json_valid_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_parse_t *p;          /* The parse */
  u_int8_t flags = 1;
  u_int8_t res = 0;
  if( argc==2 ){
    int64_t f = dbsql_value_int64(argv[1]);
    if( f<1 || f>15 ){
      dbsql_result_error(ctx, "FLAGS parameter to json_valid() must be"
                                " between 1 and 15", -1);
      return;
    }
    flags = f & 0x0f;
  }
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_NULL: {
#ifdef DBSQL_LEGACY_JSON_VALID
      /* Incorrect legacy behavior was to return FALSE for a NULL input */
      dbsql_result_int(ctx, 0);
#endif
      return;
    }
    case DBSQL_BLOB: {
      json_parse_t py;
      memset(&py, 0, sizeof(py));
      if( __json_arg_is_jsonb(argv[0], &py) ){
        if( flags & 0x04 ){
          /* Superficial checking only - accomplished by the
          ** __json_arg_is_jsonb() call above. */
          res = 1;
        }else if( flags & 0x08 ){
          /* Strict checking.  Check by translating BLOB->TEXT->BLOB.  If
          ** no errors occur, call that a "strict check". */
          res = 0==__jsonb_validity_check(&py, 0, py.n_blob, 1);
        }
        break;
      }
      /* Fall through into interpreting the input as text.  See note
      ** above at tag-20240123-a. */
      /* no break */ deliberate_fall_through
    }
    default: {
      json_parse_t px;
      if( (flags & 0x3)==0 ) break;
      memset(&px, 0, sizeof(px));
     
      p = __json_parse_func_arg(ctx, argv[0], JSON_KEEPERROR);
      if( p ){
        if( p->oom ){
          dbsql_result_error_nomem(ctx);
        }else if( p->n_err ){
          /* no-op */
        }else if( (flags & 0x02)!=0 || p->has_nonstd==0 ){
          res = 1;
        }
        __json_parse_free(p);
      }else{
        dbsql_result_error_nomem(ctx);
      }
      break;
    }
  }
  dbsql_result_int(ctx, res);
}

/*
** json_error_position(JSON)
**
** If the argument is NULL, return NULL
**
** If the argument is BLOB, do a full validity check and return non-zero
** if the check fails.  The return value is the approximate 1-based offset
** to the byte of the element that contains the first error.
**
** Otherwise interpret the argument is TEXT (even if it is numeric) and
** return the 1-based character position for where the parser first recognized
** that the input was not valid JSON, or return 0 if the input text looks
** ok.  JSON-5 extensions are accepted.
*/
static void __json_error_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  int64_t i_err_pos = 0;       /* Error position to be returned */
  json_parse_t s;

  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  memset(&s, 0, sizeof(s));
  s.db = dbsql_context_db_handle(ctx);
  if( __json_arg_is_jsonb(argv[0], &s) ){
    i_err_pos = (int64_t)__jsonb_validity_check(&s, 0, s.n_blob, 1);
  }else{
    s.json = (char*)dbsql_value_text(argv[0]);
    if( s.json==0 ) return;  /* NULL input or OOM */
    s.n_json = dbsql_value_bytes(argv[0]);
    if( __json_convert_text_to_blob(&s,0) ){
      if( s.oom ){
        i_err_pos = -1;
      }else{
        /* Convert byte-offset s.i_err into a character offset */
        u_int32_t k;
        assert( s.json!=0 );  /* Because s.oom is false */
        for(k=0; k<s.i_err && ALWAYS(s.json[k]); k++){
          if( (s.json[k] & 0xc0)!=0x80 ) i_err_pos++;
        }
        i_err_pos++;
      }
    }
  }
  __json_parse_reset(&s);
  if( i_err_pos<0 ){
    dbsql_result_error_nomem(ctx);
  }else{
    dbsql_result_int64(ctx, i_err_pos);
  }
}

/****************************************************************************
** Aggregate SQL function implementations
****************************************************************************/
/*
** json_group_array(VALUE)
**
** Return a JSON array composed of all values in the aggregate.
*/
static void __json_array_step(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_string_t *str;
  UNUSED_PARAMETER(argc);
  str = (json_string_t*)dbsql_aggregate_context(ctx, sizeof(*str));
  if( str ){
    if( str->buf==0 ){
      __json_string_init(str, ctx);
      __json_append_char(str, '[');
    }else if( str->n_used>1 ){
      __json_append_char(str, ',');
    }
    str->ctx = ctx;
    __json_append_sql_value(str, argv[0]);
  }
}
static void __json_array_compute(dbsql_func_t *ctx, int isFinal){
  json_string_t *str;
  int flags = DBSQL_PTR_TO_INT(dbsql_user_data(ctx));
  str = (json_string_t*)dbsql_aggregate_context(ctx, 0);
  if( str ){
    str->ctx = ctx;
    __json_append_raw_nz(str, "]", 2);
    __json_string_trim_one_char(str);
    if( str->err ){
      __json_return_string(str, 0, 0);
      return;
    }else if( flags & JSON_BLOB ){
      __json_return_string_as_blob(str);
      if( isFinal ){
        if( !str->b_static ) __rc_str_unref(str->buf);
      }else{
        __json_string_trim_one_char(str);
      }
      return;
    }else if( isFinal ){
      dbsql_result_text(ctx, str->buf, (int)str->n_used,
                          str->b_static ? DBSQL_TRANSIENT :
                              __rc_str_unref);
      str->b_static = 1;
    }else{
      dbsql_result_text(ctx, str->buf, (int)str->n_used, DBSQL_TRANSIENT);
      __json_string_trim_one_char(str);
    }
  }else if( flags & JSON_BLOB ){
    static const u_int8_t empty_array = 0x0b;
    dbsql_result_blob(ctx, &empty_array, 1, DBSQL_STATIC);
  }else{
    dbsql_result_text(ctx, "[]", 2, DBSQL_STATIC);
  }
  dbsql_result_subtype(ctx, JSON_SUBTYPE);
}
static void __json_array_value(dbsql_func_t *ctx){
  __json_array_compute(ctx, 0);
}
static void __json_array_final(dbsql_func_t *ctx){
  __json_array_compute(ctx, 1);
}

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** This method works for both json_group_array() and json_group_object().
** It works by removing the first element of the group by searching forward
** to the first comma (",") that is not within a string and deleting all
** text through that comma.
*/
static void JSON_GROUP_INVERSE(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  unsigned int i;
  int in_str = 0;
  int n_nest = 0;
  char *z;
  char c;
  json_string_t *str;
  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);
  str = (json_string_t*)dbsql_aggregate_context(ctx, 0);
  /* str is always non-NULL since __json_array_step() or __json_object_step() will
  ** always have been called to initialize it */
  if( NEVER(!str) ) return;
  z = str->buf;
  for(i=1; i<str->n_used && ((c = z[i])!=',' || in_str || n_nest); i++){
    if( c=='"' ){
      in_str = !in_str;
    }else if( c=='\\' ){
      i++;
    }else if( !in_str ){
      if( c=='{' || c=='[' ) n_nest++;
      if( c=='}' || c==']' ) n_nest--;
    }
  }
  if( i<str->n_used ){
    str->n_used -= i;
    memmove(&z[1], &z[i+1], (size_t)str->n_used-1);
    z[str->n_used] = 0;
  }else{
    str->n_used = 1;
  }
}
#else
# define JSON_GROUP_INVERSE 0
#endif


/*
** json_group_obj(NAME,VALUE)
**
** Return a JSON object composed of all names and values in the aggregate.
**
** Rows for which NAME is NULL do not result in a new entry.  However, we
** do initially insert a "@" entry into the growing string for each null entry
** and change the first character of the string to "@" to signal that the
** string contains null entries.  The "@" markers are needed in order to
** correctly process inverse() requests.  The initial "@" is converted
** back into "{" and the "@" null values are removed by __json_object_compute().
*/
static void __json_object_step(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  json_string_t *str;
  const char *z;
  u_int32_t n;
  UNUSED_PARAMETER(argc);
  str = (json_string_t*)dbsql_aggregate_context(ctx, sizeof(*str));
  if( str ){
    z = (const char*)dbsql_value_text(argv[0]);
    n = __strlen30(z);
    if( str->buf==0 ){
      __json_string_init(str, ctx);
      __json_append_char(str, '{');
    }else if( str->n_used>1 ){
      __json_append_char(str, ',');
    }
    str->ctx = ctx;
    if( z!=0 ){
      __json_append_string(str, z, n);
      __json_append_char(str, ':');
      __json_append_sql_value(str, argv[1]);
    }else{
      str->buf[0] = '@';
      __json_append_raw_nz(str, "@", 1);
    }
  }
}
static void __json_object_compute(dbsql_func_t *ctx, int isFinal){
  json_string_t *str;
  int flags = DBSQL_PTR_TO_INT(dbsql_user_data(ctx));
  str = (json_string_t*)dbsql_aggregate_context(ctx, 0);
  if( str ){
    json_string_t *og_str = str;
    json_string_t tmp_str;
    __json_append_raw_nz(og_str, "}", 2);  /* Ensure it is zero-terminated */
    __json_string_trim_one_char(og_str);    /* Remove the zero terminator */
    str->ctx = ctx;
    if( str->err ){
      __json_return_string(str, 0, 0);
      return;
    }
    if( str->buf[0]!='{' ){
      /* The string contains null entries that need to be removed */
      u_int64_t i, j;
      int in_str = 0;
      if( !isFinal ){
        /* Work with a temporary copy of the string if this is not the
        ** final result */
        __json_string_init(&tmp_str, ctx);
        __json_append_raw_nz(&tmp_str, str->buf, str->n_used+1);
        str = &tmp_str;
        if( str->err ){
          __json_return_string(str, 0, 0);
          return;
        }
        __json_string_trim_one_char(str);  /* Remove zero terminator */
      }
      /* Fix up the string by changing the initial "@" flag back to
      ** to "{" and removing all subsequence "@" entries, with their
      ** associated comma delimeters. */
      str->buf[0] = '{';
      for(i=j=1; i<str->n_used; i++){
        char c = str->buf[i];
        if( c=='"' ){
          in_str = !in_str;
          str->buf[j++] = '"';
        }else if( c=='\\' ){
          str->buf[j++] = '\\';
          str->buf[j++] = str->buf[++i];
        }else if( c=='@' && !in_str ){
          assert( i+1<str->n_used );
          if( str->buf[i+1]==',' ){
            i++;
          }else if( str->buf[j-1]==',' ){
            j--;
          }
        }else{
          str->buf[j++] = c;
        }
      }
      str->buf[j] = 0;  /* Restore zero terminator */
      str->n_used = j;    /* Truncate the string */
    }
    if( flags & JSON_BLOB ){
      __json_return_string_as_blob(str);
      if( isFinal ){
        if( !str->b_static ) __rc_str_unref(str->buf);
      }else{
        __json_string_trim_one_char(og_str);
      }
    }else if( isFinal ){
      dbsql_result_text(ctx, str->buf, (int)str->n_used,
                          str->b_static ? DBSQL_TRANSIENT :
                          __rc_str_unref);
      str->b_static = 1;
    }else{
      dbsql_result_text(ctx, str->buf, (int)str->n_used, DBSQL_TRANSIENT);
      __json_string_trim_one_char(og_str);
    }
    if( str!=og_str ) __json_string_reset(str);
  }else if( flags & JSON_BLOB ){
    static const unsigned char empty_object = 0x0c;
    dbsql_result_blob(ctx, &empty_object, 1, DBSQL_STATIC); 
  }else{
    dbsql_result_text(ctx, "{}", 2, DBSQL_STATIC);
  }
  dbsql_result_subtype(ctx, JSON_SUBTYPE);
}
static void __json_object_value(dbsql_func_t *ctx){
  __json_object_compute(ctx, 0);
}
static void __json_object_final(dbsql_func_t *ctx){
  __json_object_compute(ctx, 1);
}



#ifndef DBSQL_OMIT_VIRTUALTABLE
/****************************************************************************
** The json_each virtual table
****************************************************************************/
typedef struct json_parent_t json_parent_t;
struct json_parent_t {
  u_int32_t i_head;                 /* Start of object or array */
  u_int32_t i_value;                /* Start of the value */
  u_int32_t i_end;                  /* First byte past the end */
  u_int32_t n_path;                 /* Length of path */
  int64_t i_key;                  /* Key for JSONB_ARRAY */
};

typedef struct json_each_cursor_t json_each_cursor_t;
struct json_each_cursor_t {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  u_int32_t i_rowid;                /* The rowid */
  u_int32_t i;                     /* index_t in s_parse.blobs[] of current row */
  u_int32_t i_end;                  /* EOF when i equals or exceeds this value */
  u_int32_t n_root;                 /* Size of the root path in bytes */
  u_int8_t type;                  /* Type of the container for element i */
  u_int8_t recursive;             /* True for json_tree().  False for json_each() */
  u_int8_t mode;                  /* 1 for json_each().  2 for jsonb_each() */
  u_int32_t n_parent;               /* Current nesting depth */
  u_int32_t n_parent_alloc;          /* Space allocated for parents[] */
  json_parent_t *parents;       /* Parent elements of i */
  DBSQL *db;               /* Database connection */
  json_string_t path;           /* Current path */
  json_parse_t s_parse;          /* parser_t of the input JSON */
};
typedef struct json_each_connection_t json_each_connection_t;
struct json_each_connection_t {
  dbsql_vtab_t base;         /* Base class - must be first */
  DBSQL *db;               /* Database connection */
  u_int8_t mode;                  /* 1 for json_each().  2 for jsonb_each() */
  u_int8_t recursive;             /* True for json_tree().  False for json_each() */
};


/* Constructor for the json_each virtual table */
static int __json_each_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  json_each_connection_t *p_new;
  int rc;

/* column_t numbers */
#define JEACH_KEY     0
#define JEACH_VALUE   1
#define JEACH_TYPE    2
#define JEACH_ATOM    3
#define JEACH_ID      4
#define JEACH_PARENT  5
#define JEACH_FULLKEY 6
#define JEACH_PATH    7
/* The best_index method assumes that the JSON and ROOT columns are
** the last two columns in the table.  Should this ever changes, be
** sure to update the best_index method. */
#define JEACH_JSON    8
#define JEACH_ROOT    9

  UNUSED_PARAMETER(err_p);
  UNUSED_PARAMETER(argv);
  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(aux);
  rc = dbsql_declare_vtab(db,
     "CREATE TABLE x(key,value,type,atom,id,parent,fullkey,path,"
                    "json HIDDEN,root HIDDEN)");
  if( rc==DBSQL_OK ){
    p_new = (json_each_connection_t*)__db_malloc_zero(db, sizeof(*p_new));
    *vtab = (dbsql_vtab_t*)p_new;
    if( p_new==0 ) return DBSQL_NOMEM;
    dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
    p_new->db = db;
    p_new->mode = argv[0][4]=='b' ? 2 : 1;
    p_new->recursive = argv[0][4+p_new->mode]=='t';
  }
  return rc;
}

/* destructor for json_each virtual table */
static int __json_each_disconnect(dbsql_vtab_t *vtab){
  json_each_connection_t *p = (json_each_connection_t*)vtab;
  __dbsql_db_free(p->db, vtab);
  return DBSQL_OK;
}

/* constructor for a json_each_cursor_t object for json_each()/json_tree(). */
static int __json_each_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  json_each_connection_t *vtab = (json_each_connection_t*)p;
  json_each_cursor_t *cur;

  UNUSED_PARAMETER(p);
  cur = __db_malloc_zero(vtab->db, sizeof(*cur));
  if( cur==0 ) return DBSQL_NOMEM;
  cur->db = vtab->db;
  cur->mode = vtab->mode;
  cur->recursive = vtab->recursive;
  __json_string_zero(&cur->path);
  *cursor = &cur->base;
  return DBSQL_OK;
}

/* Reset a json_each_cursor_t back to its original state.  Free any memory
** held. */
static void __json_each_cursor_reset(json_each_cursor_t *p){
  __json_parse_reset(&p->s_parse);
  __json_string_reset(&p->path);
  __dbsql_db_free(p->db, p->parents);
  p->i_rowid = 0;
  p->i = 0;
  p->parents = 0;
  p->n_parent = 0;
  p->n_parent_alloc = 0;
  p->i_end = 0;
  p->type = 0;
}

/* Destructor for a json_each_cursor object */
static int __json_each_close(dbsql_vtab_cursor_t *cur){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  __json_each_cursor_reset(p);
  
  __dbsql_db_free(p->db, cur);
  return DBSQL_OK;
}

/* Return TRUE if the json_each_cursor object has been advanced off the end
** of the JSON object */
static int __json_each_eof(dbsql_vtab_cursor_t *cur){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  return p->i >= p->i_end;
}

/*
** If the cursor is currently pointing at the label of a object entry,
** then return the index of the value.  For all other cases, return the
** current pointer position, which is the value.
*/
static int __json_skip_label(json_each_cursor_t *p){
  if( p->type==JSONB_OBJECT ){
    u_int32_t sz = 0;
    u_int32_t n = __jsonb_payload_size(&p->s_parse, p->i, &sz);
    sz += p->i + n;
    if( sz >= p->s_parse.n_blob ) sz = p->i;
    return sz;
  }else{
    return p->i;
  }
}

/*
** Append the path name for the current element.
*/
static void __json_append_path_name(json_each_cursor_t *p){
  assert( p->n_parent>0 );
  assert( p->type==JSONB_ARRAY || p->type==JSONB_OBJECT );
  if( p->type==JSONB_ARRAY ){
    __json_printf(30, &p->path, "[%lld]", p->parents[p->n_parent-1].i_key);
  }else{
    u_int32_t n, sz = 0, k, i;
    const char *z;
    int need_quote = 0;
    n = __jsonb_payload_size(&p->s_parse, p->i, &sz);
    k = p->i + n;
    z = (const char*)&p->s_parse.blobs[k];
    if( sz==0 || !__isalpha(z[0]) ){
      need_quote = 1;
    }else{
      for(i=0; i<sz; i++){
        if( !__isalnum(z[i]) ){
          need_quote = 1;
          break;
        }
      }
    }
    if( need_quote ){
      __json_printf(sz+4,&p->path,".\"%.*s\"", sz, z);
    }else{
      __json_printf(sz+2,&p->path,".%.*s", sz, z);
    }
  }
}

/* Advance the cursor to the next element for json_tree() */
static int __json_each_next(dbsql_vtab_cursor_t *cur){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  int rc = DBSQL_OK;
  if( p->recursive ){
    u_int8_t x;
    u_int8_t level_change = 0;
    u_int32_t n, sz = 0;
    u_int32_t i = __json_skip_label(p);
    x = p->s_parse.blobs[i] & 0x0f;
    n = __jsonb_payload_size(&p->s_parse, i, &sz);
    if( x==JSONB_OBJECT || x==JSONB_ARRAY ){
      json_parent_t *parent;
      if( p->n_parent>=p->n_parent_alloc ){
        json_parent_t *p_new;
        u_int64_t n_new;
        n_new = p->n_parent_alloc*2 + 3;
        p_new = __db_realloc(p->db, p->parents, sizeof(json_parent_t)*n_new);
        if( p_new==0 ) return DBSQL_NOMEM;
        p->n_parent_alloc = (u_int32_t)n_new;
        p->parents = p_new;
      }
      level_change = 1;
      parent = &p->parents[p->n_parent];
      parent->i_head = p->i;
      parent->i_value = i;
      parent->i_end = i + n + sz;
      parent->i_key = -1;
      parent->n_path = (u_int32_t)p->path.n_used;
      if( p->type && p->n_parent ){
        __json_append_path_name(p);
        if( p->path.err ) rc = DBSQL_NOMEM;
      }
      p->n_parent++;
      p->i = i + n;
    }else{
      p->i = i + n + sz;
    }
    while( p->n_parent>0 && p->i >= p->parents[p->n_parent-1].i_end ){
      p->n_parent--;
      p->path.n_used = p->parents[p->n_parent].n_path;
      level_change = 1;
    }
    if( level_change ){
      if( p->n_parent>0 ){
        json_parent_t *parent = &p->parents[p->n_parent-1];
        u_int32_t i_val = parent->i_value;
        p->type = p->s_parse.blobs[i_val] & 0x0f;
      }else{
        p->type = 0;
      }
    }
  }else{
    u_int32_t n, sz = 0;
    u_int32_t i = __json_skip_label(p);
    n = __jsonb_payload_size(&p->s_parse, i, &sz);
    p->i = i + n + sz;
  }
  if( p->type==JSONB_ARRAY && p->n_parent ){
    p->parents[p->n_parent-1].i_key++;
  }
  p->i_rowid++;
  return rc;
}

/* Length of the path for rowid==0 in recursive mode.
*/
static int __json_each_path_length(json_each_cursor_t *p){
  u_int32_t n = p->path.n_used;
  char *z = p->path.buf;
  if( p->i_rowid==0 && p->recursive && n>=2 ){
    while( n>1 ){
      n--;
      if( z[n]=='[' || z[n]=='.' ){
        u_int32_t x, sz = 0;
        char c_saved = z[n];
        z[n] = 0;
        assert( p->s_parse.edit==0 );
        x = __json_lookup_step(&p->s_parse, 0, z+1, 0);
        z[n] = c_saved;
        if( JSON_LOOKUP_ISERROR(x) ) continue;
        if( x + __jsonb_payload_size(&p->s_parse, x, &sz) == p->i ) break;
      }
    }
  }
  return n;
}

/* Return the value of a column */
static int __json_each_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i_column                 /* Which column to return */
){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  switch( i_column ){
    case JEACH_KEY: {
      if( p->n_parent==0 ){
        u_int32_t n, j;
        if( p->n_root==1 ) break;
        j = __json_each_path_length(p);
        n = p->n_root - j;
        if( n==0 ){
          break;
        }else if( p->path.buf[j]=='[' ){
          int64_t x;
          __atoi64(&p->path.buf[j+1], &x, n-1, DBSQL_UTF8);
          dbsql_result_int64(ctx, x);
        }else if( p->path.buf[j+1]=='"' ){
          dbsql_result_text(ctx, &p->path.buf[j+2], n-3, DBSQL_TRANSIENT);
        }else{
          dbsql_result_text(ctx, &p->path.buf[j+1], n-1, DBSQL_TRANSIENT);
        }
        break;
      }
      if( p->type==JSONB_OBJECT ){
        __json_return_from_blob(&p->s_parse, p->i, ctx, 1);
      }else{
        assert( p->type==JSONB_ARRAY );
        dbsql_result_int64(ctx, p->parents[p->n_parent-1].i_key);
      }
      break;
    }
    case JEACH_VALUE: {
      u_int32_t i = __json_skip_label(p);
      __json_return_from_blob(&p->s_parse, i, ctx, p->mode);
      if( (p->s_parse.blobs[i] & 0x0f)>=JSONB_ARRAY ){
        dbsql_result_subtype(ctx, JSON_SUBTYPE);
      }
      break;
    }
    case JEACH_TYPE: {
      u_int32_t i = __json_skip_label(p);
      u_int8_t type = p->s_parse.blobs[i] & 0x0f;
      dbsql_result_text(ctx, __jsonb_type[type], -1, DBSQL_STATIC);
      break;
    }
    case JEACH_ATOM: {
      u_int32_t i = __json_skip_label(p);
      if( (p->s_parse.blobs[i] & 0x0f)<JSONB_ARRAY ){
        __json_return_from_blob(&p->s_parse, i, ctx, 1);
      }
      break;
    }
    case JEACH_ID: {
      dbsql_result_int64(ctx, (int64_t)p->i);
      break;
    }
    case JEACH_PARENT: {
      if( p->n_parent>0 && p->recursive ){
        dbsql_result_int64(ctx, p->parents[p->n_parent-1].i_head);
      }
      break;
    }
    case JEACH_FULLKEY: {
      u_int64_t n_base = p->path.n_used;
      if( p->n_parent ) __json_append_path_name(p);
      dbsql_result_text64(ctx, p->path.buf, p->path.n_used,
                            DBSQL_TRANSIENT, DBSQL_UTF8);
      p->path.n_used = n_base;
      break;
    }
    case JEACH_PATH: {
      u_int32_t n = __json_each_path_length(p);
      dbsql_result_text64(ctx, p->path.buf, n,
                            DBSQL_TRANSIENT, DBSQL_UTF8);
      break;
    }
    default: {
      dbsql_result_text(ctx, p->path.buf, p->n_root, DBSQL_STATIC);
      break;
    }
    case JEACH_JSON: {
      if( p->s_parse.json==0 ){
        dbsql_result_blob(ctx, p->s_parse.blobs, p->s_parse.n_blob,
                            DBSQL_TRANSIENT);
      }else{
        dbsql_result_text(ctx, p->s_parse.json, -1, DBSQL_TRANSIENT);
      }
      break;
    }
  }
  return DBSQL_OK;
}

/* Return the current rowid value */
static int __json_each_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  *rowid = p->i_rowid;
  return DBSQL_OK;
}

/* The query strategy is to look for an equality constraint on the json
** column.  Without such a constraint, the table cannot operate.  idx_num is
** 1 if the constraint is found, 3 if the constraint and root are found,
** and 0 otherwise.
*/
static int __json_each_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;                     /* Loop counter or computed array index */
  int idx[2];               /* index_t of constraints for JSON and ROOT */
  int unusable_mask = 0;      /* Mask of unusable JSON and ROOT constraints */
  int idx_mask = 0;           /* Mask of usable == constraints JSON and ROOT */
  const struct dbsql_index_constraint_t *constraint;

  /* This implementation assumes that JSON and ROOT are the last two
  ** columns in the table */
  assert( JEACH_ROOT == JEACH_JSON+1 );
  UNUSED_PARAMETER(tab);
  idx[0] = idx[1] = -1;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    int i_col;
    int i_mask;
    if( constraint->i_column < JEACH_JSON ) continue;
    i_col = constraint->i_column - JEACH_JSON;
    assert( i_col==0 || i_col==1 );
    testcase( i_col==0 );
    i_mask = 1 << i_col;
    if( constraint->usable==0 ){
      unusable_mask |= i_mask;
    }else if( constraint->op==DBSQL_INDEX_CONSTRAINT_EQ ){
      idx[i_col] = i;
      idx_mask |= i_mask;
    }
  }
  if( idx_info->n_order_by>0
   && idx_info->order_bies[0].i_column<0
   && idx_info->order_bies[0].desc==0
  ){
    idx_info->order_by_consumed = 1;
  }

  if( (unusable_mask & ~idx_mask)!=0 ){
    /* If there are any unusable constraints on JSON or ROOT, then reject
    ** this entire plan */
    return DBSQL_CONSTRAINT;
  }
  if( idx[0]<0 ){
    /* No JSON input.  Leave estimated_cost at the huge value that it was
    ** initialized to to discourage the query planner from selecting this
    ** plan. */
    idx_info->idx_num = 0;
  }else{
    idx_info->estimated_cost = 1.0;
    i = idx[0];
    idx_info->constraint_usages[i].argv_index = 1;
    idx_info->constraint_usages[i].omit = 1;
    if( idx[1]<0 ){
      idx_info->idx_num = 1;  /* Only JSON supplied.  Plan 1 */
    }else{
      i = idx[1];
      idx_info->constraint_usages[i].argv_index = 2;
      idx_info->constraint_usages[i].omit = 1;
      idx_info->idx_num = 3;  /* Both JSON and ROOT are supplied.  Plan 3 */
    }
  }
  return DBSQL_OK;
}

/* Start a search on a new JSON string */
static int __json_each_filter(
  dbsql_vtab_cursor_t *cur,
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  json_each_cursor_t *p = (json_each_cursor_t*)cur;
  const char *root = 0;
  u_int32_t i, n, sz;

  UNUSED_PARAMETER(idx_str);
  UNUSED_PARAMETER(argc);
  __json_each_cursor_reset(p);
  if( idx_num==0 ) return DBSQL_OK;
  memset(&p->s_parse, 0, sizeof(p->s_parse));
  p->s_parse.n_jp_ref = 1;
  p->s_parse.db = p->db;
  if( __json_arg_is_jsonb(argv[0], &p->s_parse) ){
    /* We have JSONB */
  }else{
    p->s_parse.json = (char*)dbsql_value_text(argv[0]);
    p->s_parse.n_json = dbsql_value_bytes(argv[0]);
    if( p->s_parse.json==0 ){
      p->i = p->i_end = 0;
      return DBSQL_OK;
    }      
    if( __json_convert_text_to_blob(&p->s_parse, 0) ){
      if( p->s_parse.oom ){
        return DBSQL_NOMEM;
      }
      goto json_each_malformed_input;
    }
  }
  if( idx_num==3 ){
    root = (const char*)dbsql_value_text(argv[1]);
    if( root==0 ) return DBSQL_OK;
    if( root[0]!='$' ){
      dbsql_free(cur->vtab->errmsg);
      cur->vtab->errmsg = __json_bad_path_error(0, root, 0);
      __json_each_cursor_reset(p);
      return cur->vtab->errmsg ? DBSQL_ERROR : DBSQL_NOMEM;
    }
    p->n_root = __strlen30(root);
    if( root[1]==0 ){
      i = p->i = 0;
      p->type = 0;
    }else{
      i = __json_lookup_step(&p->s_parse, 0, root+1, 0);
      if( JSON_LOOKUP_ISERROR(i) ){
        if( i==JSON_LOOKUP_NOTFOUND ){
          p->i = 0;
          p->type = 0;
          p->i_end = 0;
          return DBSQL_OK;
        }
        dbsql_free(cur->vtab->errmsg);
        cur->vtab->errmsg = __json_bad_path_error(0, root, 0);
        __json_each_cursor_reset(p);
        return cur->vtab->errmsg ? DBSQL_ERROR : DBSQL_NOMEM;
      }
      if( p->s_parse.i_label ){
        p->i = p->s_parse.i_label;
        p->type = JSONB_OBJECT;
      }else{
        p->i = i;
        p->type = JSONB_ARRAY;
      }
    }
    __json_append_raw(&p->path, root, p->n_root);
  }else{
    i = p->i = 0;
    p->type = 0;
    p->n_root = 1;
    __json_append_raw(&p->path, "$", 1);
  }
  p->n_parent = 0;
  n = __jsonb_payload_size(&p->s_parse, i, &sz);
  p->i_end = i+n+sz;
  if( (p->s_parse.blobs[i] & 0x0f)>=JSONB_ARRAY && !p->recursive ){
    p->i = i + n;
    p->type = p->s_parse.blobs[i] & 0x0f;
    p->parents = __db_malloc_zero(p->db, sizeof(json_parent_t));
    if( p->parents==0 ) return DBSQL_NOMEM;
    p->n_parent = 1;
    p->n_parent_alloc = 1;
    p->parents[0].i_key = 0;
    p->parents[0].i_end = p->i_end;
    p->parents[0].i_head = p->i;
    p->parents[0].i_value = i;
  }
  return DBSQL_OK;

json_each_malformed_input:
  dbsql_free(cur->vtab->errmsg);
  cur->vtab->errmsg = dbsql_mprintf("malformed JSON");
  __json_each_cursor_reset(p);
  return cur->vtab->errmsg ? DBSQL_ERROR : DBSQL_NOMEM;
}

/* The methods of the json_each virtual table */
static dbsql_module_t __json_each_module = {
  0,                         /* i_version */
  0,                         /* create */
  __json_each_connect,           /* connect */
  __json_each_best_index,         /* best_index */
  __json_each_disconnect,        /* disconnect */
  0,                         /* destroy */
  __json_each_open,              /* x_open - open a cursor */
  __json_each_close,             /* x_close - close a cursor */
  __json_each_filter,            /* filter - configure scan constraints */
  __json_each_next,              /* next - advance a cursor */
  __json_each_eof,               /* eof - check for end of scan */
  __json_each_column,            /* column - read data */
  __json_each_rowid,             /* rowid - read data */
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
  0,                         /* shadow_name */
  0                          /* integrity */
};
#endif /* DBSQL_OMIT_VIRTUALTABLE */
#endif /* !defined(DBSQL_OMIT_JSON) */

/*
** Register JSON functions.
*/
void __register_json_functions(void){
#ifndef DBSQL_OMIT_JSON
  static func_def_t json_funcs[] = {
    /*   dbsql_result_subtype() ----,  ,--- dbsql_value_subtype()       */
    /*                                |  |                                  */
    /*             Uses cache ------, |  | ,---- Returns JSONB              */
    /*                              | |  | |                                */
    /*     Number of arguments ---, | |  | | ,--- Flags                     */
    /*                            | | |  | | |                              */
    JFUNCTION(json,               1,1,1, 0,0,0,          __json_remove_func),
    JFUNCTION(jsonb,              1,1,0, 0,1,0,          __json_remove_func),
    JFUNCTION(json_array,        -1,0,1, 1,0,0,          __json_array_func),
    JFUNCTION(jsonb_array,       -1,0,1, 1,1,0,          __json_array_func),
    JFUNCTION(json_array_insert, -1,1,1, 1,0,JSON_AINS,  __json_set_func),
    JFUNCTION(jsonb_array_insert,-1,1,0, 1,1,JSON_AINS,  __json_set_func),
    JFUNCTION(json_array_length,  1,1,0, 0,0,0,          __json_array_length_func),
    JFUNCTION(json_array_length,  2,1,0, 0,0,0,          __json_array_length_func),
    JFUNCTION(json_error_position,1,1,0, 0,0,0,          __json_error_func),
    JFUNCTION(json_extract,      -1,1,1, 0,0,0,          __json_extract_func),
    JFUNCTION(jsonb_extract,     -1,1,0, 0,1,0,          __json_extract_func),
    JFUNCTION(->,                 2,1,1, 0,0,JSON_JSON,  __json_extract_func),
    JFUNCTION(->>,                2,1,0, 0,0,JSON_SQL,   __json_extract_func),
    JFUNCTION(json_insert,       -1,1,1, 1,0,0,          __json_set_func),
    JFUNCTION(jsonb_insert,      -1,1,0, 1,1,0,          __json_set_func),
    JFUNCTION(json_object,       -1,0,1, 1,0,0,          __json_object_func),
    JFUNCTION(jsonb_object,      -1,0,1, 1,1,0,          __json_object_func),
    JFUNCTION(json_patch,         2,1,1, 0,0,0,          __json_patch_func),
    JFUNCTION(jsonb_patch,        2,1,0, 0,1,0,          __json_patch_func),
    JFUNCTION(json_pretty,        1,1,0, 0,0,0,          __json_pretty_func),
    JFUNCTION(json_pretty,        2,1,0, 0,0,0,          __json_pretty_func),
    JFUNCTION(json_quote,         1,0,1, 1,0,0,          __json_quote_func),
    JFUNCTION(json_remove,       -1,1,1, 0,0,0,          __json_remove_func),
    JFUNCTION(jsonb_remove,      -1,1,0, 0,1,0,          __json_remove_func),
    JFUNCTION(json_replace,      -1,1,1, 1,0,0,          __json_replace_func),
    JFUNCTION(jsonb_replace,     -1,1,0, 1,1,0,          __json_replace_func),
    JFUNCTION(json_set,          -1,1,1, 1,0,JSON_ISSET, __json_set_func),
    JFUNCTION(jsonb_set,         -1,1,0, 1,1,JSON_ISSET, __json_set_func),
    JFUNCTION(json_type,          1,1,0, 0,0,0,          __json_type_func),
    JFUNCTION(json_type,          2,1,0, 0,0,0,          __json_type_func),
    JFUNCTION(json_valid,         1,1,0, 0,0,0,          __json_valid_func),
    JFUNCTION(json_valid,         2,1,0, 0,0,0,          __json_valid_func),
#if DBSQL_DEBUG
    JFUNCTION(json_parse,         1,1,0, 0,0,0,          __json_parse_func),
#endif
    WAGGREGATE(json_group_array,  1, 0, 0,
       __json_array_step, __json_array_final, __json_array_value, JSON_GROUP_INVERSE,
       DBSQL_SUBTYPE|DBSQL_RESULT_SUBTYPE|DBSQL_UTF8|
       DBSQL_DETERMINISTIC),
    WAGGREGATE(jsonb_group_array, 1, JSON_BLOB, 0,
       __json_array_step, __json_array_final, __json_array_value, JSON_GROUP_INVERSE,
       DBSQL_SUBTYPE|DBSQL_RESULT_SUBTYPE|DBSQL_UTF8|DBSQL_DETERMINISTIC),
    WAGGREGATE(json_group_object, 2, 0, 0,
       __json_object_step, __json_object_final, __json_object_value, JSON_GROUP_INVERSE,
       DBSQL_SUBTYPE|DBSQL_RESULT_SUBTYPE|DBSQL_UTF8|DBSQL_DETERMINISTIC),
    WAGGREGATE(jsonb_group_object,2, JSON_BLOB, 0,
       __json_object_step, __json_object_final, __json_object_value, JSON_GROUP_INVERSE,
       DBSQL_SUBTYPE|DBSQL_RESULT_SUBTYPE|DBSQL_UTF8|
       DBSQL_DETERMINISTIC)
  };
  __insert_builtin_funcs(json_funcs, ARRAY_SIZE(json_funcs));
#endif
}

#if  !defined(DBSQL_OMIT_VIRTUALTABLE) && !defined(DBSQL_OMIT_JSON)
/*
** Register the JSON table-valued function named name and return a
** pointer to its module_def_t object.  Return NULL if something goes wrong.
*/
module_def_t *__json_vtab_register(DBSQL *db, const char *name){
  unsigned int i;
  static const char *modules[] = {
    "json_each", "json_tree", "jsonb_each", "jsonb_tree"
  };
  assert( __hash_find(&db->modules, name)==0 );
  for(i=0; i<sizeof(modules)/sizeof(modules[0]); i++){
    if( __str_icmp(modules[i],name)==0 ){
      return __vtab_create_module(db, modules[i], &__json_each_module, 0, 0);
    }
  }
  return 0;
}
#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) && !defined(DBSQL_OMIT_JSON) */
