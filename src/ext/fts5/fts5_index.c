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
** Low level access to the FTS index stored in the database file. The 
** routines in this file file implement all read and write access to the
** %_data table. Other parts of the system access this functionality via
** the interface defined in fts5Int.h.
*/


#include "fts5_int.h"

/*
** Overview:
**
** The %_data table contains all the FTS indexes for an FTS5 virtual table.
** As well as the main term index, there may be up to 31 prefix indexes.
** The format is similar to FTS3/4, except that:
**
**   * all segment b-tree leaf data is stored in fixed size page records 
**     (e.g. 1000 bytes). A single doclist may span multiple pages. Care is 
**     taken to ensure it is possible to iterate in either direction through 
**     the entries in a doclist, or to seek to a specific entry within a 
**     doclist, without loading it into memory.
**
**   * large doclists that span many pages have associated "doclist index"
**     records that contain a copy of the first rowid on each page spanned by
**     the doclist. This is used to speed up seek operations, and merges of
**     large doclists with very small doclists.
**
**   * extra fields in the "structure record" record the state of ongoing
**     incremental merge operations.
**
*/


#define FTS5_OPT_WORK_UNIT  1000  /* Number of leaf pages per optimize step */
#define FTS5_WORK_UNIT      64    /* Number of leaf pages in unit of work */

#define FTS5_MIN_DLIDX_SIZE 4     /* Add dlidx if this many empty pages */

#define FTS5_MAIN_PREFIX '0'

#if FTS5_MAX_PREFIX_INDEXES > 31
# error "FTS5_MAX_PREFIX_INDEXES is too large"
#endif

#define FTS5_MAX_LEVEL 64

/*
** There are two versions of the format used for the structure record:
**
**   1. the legacy format, that may be read by all fts5 versions, and
**
**   2. the V2 format, which is used by contentless_delete=1 databases.
**
** Both begin with a 4-byte "configuration cookie" value. Then, a legacy
** format structure record contains a varint - the number of levels in
** the structure. Whereas a V2 structure record contains the constant
** 4 bytes [0xff 0x00 0x00 0x01]. This is unambiguous as the value of a
** varint has to be at least 16256 to begin with "0xFF". And the default
** maximum number of levels is 64. 
**
** See below for more on structure record formats.
*/
#define FTS5_STRUCTURE_V2 "\xFF\x00\x00\x01"

/*
** Details:
**
** The %_data table managed by this module,
**
**     CREATE TABLE %_data(id INTEGER PRIMARY KEY, block BLOB);
**
** , contains the following 6 types of records. See the comments surrounding
** the FTS5_*_ROWID macros below for a description of how %_data rowids are 
** assigned to each fo them.
**
** 1. Structure Records:
**
**   The set of segments that make up an index - the index structure - are
**   recorded in a single record within the %_data table. The record consists
**   of a single 32-bit configuration cookie value followed by a list of
**   dbsql varints. 
**
**   If the structure record is a V2 record, the configuration cookie is 
**   followed by the following 4 bytes: [0xFF 0x00 0x00 0x01]. 
**
**   Next, the record continues with three varints:
**
**     + number of levels,
**     + total number of segments on all levels,
**     + value of write counter.
**
**   Then, for each level from 0 to n_max:
**
**     + number of input segments in ongoing merge.
**     + total number of segments in level.
**     + for each segment from oldest to newest:
**         + segment id (always > 0)
**         + first leaf page number (often 1, always greater than 0)
**         + final leaf page number
**
**      Then, for V2 structures only:
**
**         + lower origin counter value,
**         + upper origin counter value,
**         + the number of tombstone hash pages.
**
** 2. The Averages Record:
**
**   A single record within the %_data table. The data is a list of varints.
**   The first value is the number of rows in the index. Then, for each column
**   from left to right, the total number of tokens in the column for all
**   rows of the table.
**
** 3. Segment leaves:
**
**   TERM/DOCLIST FORMAT:
**
**     Most of each segment leaf is taken up by term/doclist data. The 
**     general format of term/doclist, starting with the first term
**     on the leaf page, is:
**
**         varint : size of first term
**         blob:    first term data
**         doclist: first doclist
**         zero-or-more {
**           varint:  number of bytes in common with previous term
**           varint:  number of bytes of new term data (n_new)
**           blob:    n_new bytes of new term data
**           doclist: next doclist
**         }
**
**     doclist format:
**
**         varint:  first rowid
**         poslist: first poslist
**         zero-or-more {
**           varint:  rowid delta (always > 0)
**           poslist: next poslist
**         }
**
**     poslist format:
**
**         varint: size of poslist in bytes multiplied by 2, not including
**                 this field. Plus 1 if this entry carries the "delete" flag.
**         collist: collist for column 0
**         zero-or-more {
**           0x01 byte
**           varint: column number (I)
**           collist: collist for column I
**         }
**
**     collist format:
**
**         varint: first offset + 2
**         zero-or-more {
**           varint: offset delta + 2
**         }
**
**   PAGE FORMAT
**
**     Each leaf page begins with a 4-byte header containing 2 16-bit 
**     unsigned integer fields in big-endian format. They are:
**
**       * The byte offset of the first rowid on the page, if it exists
**         and occurs before the first term (otherwise 0).
**
**       * The byte offset of the start of the page footer. If the page
**         footer is 0 bytes in size, then this field is the same as the
**         size of the leaf page in bytes.
**
**     The page footer consists of a single varint for each term located
**     on the page. Each varint is the byte offset of the current term
**     within the page, delta-compressed against the previous value. In
**     other words, the first varint in the footer is the byte offset of
**     the first term, the second is the byte offset of the second less that
**     of the first, and so on.
**
**     The term/doclist format described above is accurate if the entire
**     term/doclist data fits on a single leaf page. If this is not the case,
**     the format is changed in two ways:
**
**       + if the first rowid on a page occurs before the first term, it
**         is stored as a literal value:
**
**             varint:  first rowid
**
**       + the first term on each page is stored in the same way as the
**         very first term of the segment:
**
**             varint : size of first term
**             blob:    first term data
**
** 5. Segment doclist indexes:
**
**   Doclist indexes are themselves b-trees, however they usually consist of
**   a single leaf record only. The format of each doclist index leaf page 
**   is:
**
**     * Flags byte. Bits are:
**         0x01: Clear if leaf is also the root page, otherwise set.
**
**     * Page number of fts index leaf page. As a varint.
**
**     * First rowid on page indicated by previous field. As a varint.
**
**     * A list of varints, one for each subsequent termless page. A 
**       positive delta if the termless page contains at least one rowid, 
**       or an 0x00 byte otherwise.
**
**   Internal doclist index nodes are:
**
**     * Flags byte. Bits are:
**         0x01: Clear for root page, otherwise set.
**
**     * Page number of first child page. As a varint.
**
**     * Copy of first rowid on page indicated by previous field. As a varint.
**
**     * A list of delta-encoded varints - the first rowid on each subsequent
**       child page. 
**
** 6. Tombstone hash_t Page
**
**   These records are only ever present in contentless_delete=1 tables. 
**   There are zero or more of these associated with each segment. They
**   are used to store the tombstone rowids for rows contained in the
**   associated segments.
**
**   The set of n_hash_pg tombstone hash pages associated with a single 
**   segment together form a single hash table containing tombstone rowids.
**   To find the page of the hash on which a key might be stored:
**
**       i_pg = (rowid % n_hash_pg)
**
**   Then, within page i_pg, which has n_slot slots:
**
**       i_slot = (rowid / n_hash_pg) % n_slot
**
**   Each tombstone hash page begins with an 8 byte header: 
**
**     1-byte:  Key-size (the size in bytes of each slot). Either 4 or 8.
**     1-byte:  rowid-0-tombstone flag. This flag is only valid on the 
**              first tombstone hash page for each segment (i_pg=0). If set,
**              the hash table contains rowid 0. If clear, it does not.
**              Rowid 0 is handled specially.
**     2-bytes: unused.
**     4-bytes: Big-endian integer containing number of entries on page.
**
**   Following this are n_slot 4 or 8 byte slots (depending on the key-size
**   in the first byte of the page header). The number of slots may be
**   determined based on the size of the page record and the key-size:
**
**     n_slot = (n_byte - 8) / key-size
*/

/*
** Rowids for the averages and structure records in the %_data table.
*/
#define FTS5_AVERAGES_ROWID     1    /* Rowid used for the averages record */
#define FTS5_STRUCTURE_ROWID   10    /* The structure record */

/*
** Macros determining the rowids used by segment leaves and dlidx leaves
** and nodes. All nodes and leaves are stored in the %_data table with large
** positive rowids.
**
** Each segment has a unique non-zero 16-bit id.
**
** The rowid for each segment leaf is found by passing the segment id and 
** the leaf page number to the FTS5_SEGMENT_ROWID macro. Leaves are numbered
** sequentially starting from 1.
*/
#define FTS5_DATA_ID_B     16     /* Max seg id number 65535 */
#define FTS5_DATA_DLI_B     1     /* Doclist-index flag (1 bit) */
#define FTS5_DATA_HEIGHT_B  5     /* Max dlidx tree height of 32 */
#define FTS5_DATA_PAGE_B   31     /* Max page number of 2147483648 */

#define fts5_dri(segid, dlidx, height, pgno) (                                 \
 ((int64_t)(segid)  << (FTS5_DATA_PAGE_B+FTS5_DATA_HEIGHT_B+FTS5_DATA_DLI_B)) +    \
 ((int64_t)(dlidx)  << (FTS5_DATA_PAGE_B + FTS5_DATA_HEIGHT_B)) +                  \
 ((int64_t)(height) << (FTS5_DATA_PAGE_B)) +                                       \
 ((int64_t)(pgno))                                                                 \
)

#define FTS5_SEGMENT_ROWID(segid, pgno)       fts5_dri(segid, 0, 0, pgno)
#define FTS5_DLIDX_ROWID(segid, height, pgno) fts5_dri(segid, 1, height, pgno)
#define FTS5_TOMBSTONE_ROWID(segid,ipg)       fts5_dri(segid+(1<<16), 0, 0, ipg)

#ifdef DBSQL_DEBUG
int __fts5_corrupt() { return DBSQL_CORRUPT_VTAB; }
#endif


/*
** Each time a blob is read from the %_data table, it is padded with this
** many zero bytes. This makes it easier to decode the various record formats
** without overreading if the records are corrupt.
*/
#define FTS5_DATA_ZERO_PADDING 8
#define FTS5_DATA_PADDING 20

typedef struct fts5_data_t fts5_data_t;
typedef struct fts5_dlidx_iter_t fts5_dlidx_iter_t;
typedef struct fts5_dlidx_lvl_t fts5_dlidx_lvl_t;
typedef struct fts5_dlidx_writer_t fts5_dlidx_writer_t;
typedef struct fts5_iter_t fts5_iter_t;
typedef struct fts5_page_writer_t fts5_page_writer_t;
typedef struct fts5_seg_iter_t fts5_seg_iter_t;
typedef struct fts5_doclist_iter_t fts5_doclist_iter_t;
typedef struct fts5_seg_writer_t fts5_seg_writer_t;
typedef struct fts5_structure_t fts5_structure_t;
typedef struct fts5_structure_level_t fts5_structure_level_t;
typedef struct fts5_structure_segment_t fts5_structure_segment_t;
typedef struct fts5_token_data_iter_t fts5_token_data_iter_t;
typedef struct fts5_token_data_map_t fts5_token_data_map_t;
typedef struct fts5_tombstone_array_t fts5_tombstone_array_t;

struct fts5_data_t {
  u_int8_t *p;                          /* Pointer to buffer containing record */
  int nn;                         /* Size of record in bytes */
  int sz_leaf;                     /* Size of leaf without page-index */
};

/*
** One object per %_data table.
**
** n_contentless_delete:
**   The number of contentless delete operations since the most recent
**   call to __fts5_index_flush() or __fts5_index_discard_data(). This is tracked
**   so that extra auto-merge work can be done by __fts5_index_flush() to
**   account for the delete operations.
*/
struct fts5_index_t {
  fts5_config_t *config;            /* Virtual table configuration */
  char *data_tbl;                 /* Name of %_data table */
  int n_work_unit;                  /* Leaf pages in a "unit" of work */

  /*
  ** Variables related to the accumulation of tokens and doclists within the
  ** in-memory hash tables before they are flushed to disk.
  */
  fts5_hash_t *hash;                /* hash_t table for in-memory data */
  int n_pending_data;               /* Current bytes of pending data */
  int64_t i_write_rowid;                /* Rowid for current doc being written */
  int b_delete;                    /* Current write is a delete */
  int n_contentless_delete;         /* Number of contentless delete ops */
  int n_pending_row;                /* Number of INSERT in hash table */

  /* Error state. */
  int rc;                         /* Current error code */
  int flush_rc;

  /* State used by the fts5_data_xxx() functions. */
  dbsql_blob_t *reader;          /* RO incr-blob open on %_data table */
  dbsql_stmt_t *writer;          /* "INSERT ... %_data VALUES(?,?)" */
  dbsql_stmt_t *deleter;         /* "DELETE FROM %_data ... id>=? AND id<=?" */
  dbsql_stmt_t *idx_writer;       /* "INSERT ... %_idx VALUES(?,?,?,?)" */
  dbsql_stmt_t *idx_deleter;      /* "DELETE FROM %_idx WHERE segid=?" */
  dbsql_stmt_t *idx_select;
  dbsql_stmt_t *idx_next_select;
  int n_read;                      /* Total number of blocks read */

  dbsql_stmt_t *delete_from_idx;

  dbsql_stmt_t *data_version;
  int64_t i_struct_version;             /* data_version when p_struct read */
  fts5_structure_t *p_struct;         /* Current db structure (or NULL) */
};

struct fts5_doclist_iter_t {
  u_int8_t *eofs;                       /* Pointer to 1 byte past end of doclist */

  /* Output variables. poslists==0 at EOF */
  int64_t i_rowid;
  u_int8_t *poslists;
  int n_poslist;
  int n_size;
};

/*
** The contents of the "structure" record for each index are represented
** using an fts5_structure_t record in memory. Which uses instances of the 
** other __fts5_structure_xxx types as components.
**
** n_origin_cntr:
**   This value is set to non-zero for structure records created for
**   contentlessdelete=1 tables only. In that case it represents the
**   origin value to apply to the next top-level segment created.
*/
struct fts5_structure_segment_t {
  int i_segid;                     /* Segment id */
  int pgno_first;                  /* First leaf page number in segment */
  int pgno_last;                   /* Last leaf page number in segment */

  /* contentlessdelete=1 tables only: */
  u_int64_t i_origin1;
  u_int64_t i_origin2;
  int n_pg_tombstone;               /* Number of tombstone hash table pages */
  u_int64_t n_entry_tombstone;            /* Number of tombstone entries that "count" */
  u_int64_t n_entry;                     /* Number of rows in this segment */
};
struct fts5_structure_level_t {
  int n_merge;                     /* Number of segments in incr-merge */
  int n_seg;                       /* Total number of segments on level */
  fts5_structure_segment_t *segs;     /* Array of segments. segs[0] is oldest. */
};
struct fts5_structure_t {
  int n_ref;                       /* Object reference count */
  u_int64_t n_write_counter;              /* Total leaves written to level 0 */
  u_int64_t n_origin_cntr;                /* Origin value for next top-level segment */
  int n_segment;                   /* Total segments in this structure */
  int n_level;                     /* Number of levels in this index */
  fts5_structure_level_t levels[FLEXARRAY]; /* Array of n_level level objects */
};

/* Size (in bytes) of an fts5_structure_t object holding up to N levels */
#define SZ_FTS5STRUCTURE(N) \
         (offsetof(fts5_structure_t,levels) + (N)*sizeof(fts5_structure_level_t))

/*
** An object of type fts5_seg_writer_t is used to write to segments.
*/
struct fts5_page_writer_t {
  int pgno;                       /* Page number for this page */
  int i_prev_pgidx;                 /* Previous value written into pgidx */
  fts5_buffer_t buf;                 /* Buffer containing leaf data */
  fts5_buffer_t pgidx;               /* Buffer containing page-index */
  fts5_buffer_t term;                /* Buffer containing previous term on page */
};
struct fts5_dlidx_writer_t {
  int pgno;                       /* Page number for this page */
  int prev_valid;                 /* True if i_prev is valid */
  int64_t i_prev;                      /* Previous rowid value written to page */
  fts5_buffer_t buf;                 /* Buffer containing page data */
};
struct fts5_seg_writer_t {
  int i_segid;                     /* Segid to write to */
  fts5_page_writer_t writer;          /* __page_writer object */
  int64_t i_prev_rowid;                 /* Previous rowid written to current leaf */
  u_int8_t first_rowid_in_doclist;        /* True if next rowid is first in doclist */
  u_int8_t first_rowid_in_page;           /* True if next rowid is first in page */
  /* TODO1: Can use (writer.pgidx.n==0) instead of first_term_in_page */
  u_int8_t first_term_in_page;            /* True if next term will be first in leaf */
  int n_leaf_written;               /* Number of leaf pages written */
  int n_empty;                     /* Number of contiguous term-less nodes */

  int n_dlidx;                     /* Allocated size of dlidxes[] array */
  fts5_dlidx_writer_t *dlidxes;        /* Array of fts5_dlidx_writer_t objects */

  /* Values to insert into the %_idx table */
  fts5_buffer_t btterm;              /* Next term to insert into %_idx table */
  int i_bt_page;                    /* Page number corresponding to btterm */
};

typedef struct fts5_c_result_t fts5_c_result_t;
struct fts5_c_result_t {
  u_int16_t i_first;                     /* segs[] index of firstest iterator */
  u_int8_t term_eq;                     /* True if the terms are equal */
};

/*
** Object for iterating through a single segment, visiting each term/rowid
** pair in the segment.
**
** seg:
**   The segment to iterate through.
**
** i_leaf_pgno:
**   Current leaf page number within segment.
**
** i_leaf_offset:
**   Byte offset within the current leaf that is the first byte of the 
**   position list data (one byte passed the position-list size field).
**
** leaf:
**   Buffer containing current leaf page data. Set to NULL at EOF.
**
** i_term_leaf_pgno, i_term_leaf_offset:
**   Leaf page number containing the last term read from the segment. And
**   the offset immediately following the term data.
**
** flags:
**   Mask of FTS5_SEGITER_XXX values. Interpreted as follows:
**
**   FTS5_SEGITER_ONETERM:
**     If set, set the iterator to point to EOF after the current doclist 
**     has been exhausted. Do not proceed to the next term in the segment.
**
**   FTS5_SEGITER_REVERSE:
**     This flag is only ever set if FTS5_SEGITER_ONETERM is also set. If
**     it is set, iterate through rowid in descending order instead of the
**     default ascending order.
**
** i_rowid_offset/n_rowid_offset/rowid_offsets:
**     These are used if the FTS5_SEGITER_REVERSE flag is set.
**
**     For each rowid on the page corresponding to the current term, the
**     corresponding rowid_offsets[] entry is set to the byte offset of the
**     start of the "position-list-size" field within the page.
**
** i_term_idx:
**     index_t of current term on i_term_leaf_pgno.
**
** tombstones/n_tombstone:
**     These are used for contentless_delete=1 tables only. When the cursor
**     is first allocated, the tombstones[] array is allocated so that it
**     is large enough for all tombstones hash pages associated with the
**     segment. The pages themselves are loaded lazily from the database as
**     they are required.
*/
struct fts5_seg_iter_t {
  fts5_structure_segment_t *seg;     /* Segment to iterate through */
  int flags;                      /* Mask of configuration flags */
  int i_leaf_pgno;                  /* Current leaf page number */
  fts5_data_t *leaf;                /* Current leaf data */
  fts5_data_t *next_leaf;            /* Leaf page (i_leaf_pgno+1) */
  int64_t i_leaf_offset;                /* Byte offset within current leaf */
  fts5_tombstone_array_t *tomb_array; /* Array of tombstone pages */

  /* Next method */
  void (*next)(fts5_index_t*, fts5_seg_iter_t*, int*);

  /* The page and offset from which the current term was read. The offset 
  ** is the offset of the first rowid in the current doclist.  */
  int i_term_leaf_pgno;
  int i_term_leaf_offset;

  int i_pgidx_off;                  /* Next offset in pgidx */
  int i_endof_doclist;

  /* The following are only used if the FTS5_SEGITER_REVERSE flag is set. */
  int i_rowid_offset;               /* Current entry in rowid_offsets[] */
  int n_rowid_offset;               /* Allocated size of rowid_offsets[] array */
  int *rowid_offsets;              /* Array of offset to rowid fields */

  fts5_dlidx_iter_t *dlidx;          /* If there is a doclist-index */

  /* Variables populated based on current entry. */
  fts5_buffer_t term;                /* Current term */
  int64_t i_rowid;                     /* Current rowid */
  int n_pos;                       /* Number of bytes in current position list */
  u_int8_t del;                        /* True if the delete flag is set */
};

static int __fts5_index_corrupt_rowid(fts5_index_t *idx, int64_t i_rowid){
  idx->rc = FTS5_CORRUPT;
  __fts5_config_errmsg(idx->config, 
      "fts5: corruption found reading blob %lld from table \"%s\"", 
      i_rowid, idx->config->name
  );
  return DBSQL_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_ROWID(idx, i_rowid) __fts5_index_corrupt_rowid(idx, i_rowid)

static int __fts5_index_corrupt_iter(fts5_index_t *idx, fts5_seg_iter_t *iter){
  idx->rc = FTS5_CORRUPT;
  __fts5_config_errmsg(idx->config, 
      "fts5: corruption on page %d, segment %d, table \"%s\"", 
      iter->i_leaf_pgno, iter->seg->i_segid, idx->config->name
  );
  return DBSQL_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_ITER(idx, iter) __fts5_index_corrupt_iter(idx, iter)

static int __fts5_index_corrupt_idx(fts5_index_t *idx){
  idx->rc = FTS5_CORRUPT;
  __fts5_config_errmsg(idx->config, 
      "fts5: corruption in table \"%s\"", idx->config->name
  );
  return DBSQL_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_IDX(idx) __fts5_index_corrupt_idx(idx)


/*
** Array of tombstone pages. Reference counted.
*/
struct fts5_tombstone_array_t {
  int n_ref;                         /* Number of pointers to this object */
  int n_tombstone;
  fts5_data_t *tombstones[FLEXARRAY]; /* Array of tombstone pages */
};

/* Size (in bytes) of an fts5_tombstone_array_t holding up to N tombstones */
#define SZ_FTS5TOMBSTONEARRAY(N) \
  (offsetof(fts5_tombstone_array_t,tombstones)+(N)*sizeof(fts5_data_t*))

/*
** Argument is a pointer to an fts5_data_t structure that contains a 
** leaf page.
*/
#define ASSERT_SZLEAF_OK(x) assert( \
    (x)->sz_leaf==(x)->nn || (x)->sz_leaf==__fts5_get_u16(&(x)->p[2]) \
)

#define FTS5_SEGITER_ONETERM 0x01
#define FTS5_SEGITER_REVERSE 0x02

/* 
** Argument is a pointer to an fts5_data_t structure that contains a leaf
** page. This macro evaluates to true if the leaf contains no terms, or
** false if it contains at least one term.
*/
#define FTS5_LEAF_IS_TERMLESS(x) ((x)->sz_leaf >= (x)->nn)

#define FTS5_LEAF_TERM_OFF(x, i) (__fts5_get_u16(&(x)->p[(x)->sz_leaf + (i)*2]))

#define FTS5_LEAF_FIRST_ROWID_OFF(x) (__fts5_get_u16((x)->p))

/*
** Object for iterating through the merged results of one or more segments,
** visiting each term/rowid pair in the merged data.
**
** n_seg is always a power of two greater than or equal to the number of
** segments that this object is merging data from. Both the segs[] and
** firsts[] arrays are sized at n_seg entries. The segs[] array is padded
** with zeroed objects - these are handled as if they were iterators opened
** on empty segments.
**
** The results of comparing segments segs[N] and segs[N+1], where N is an
** even number, is stored in firsts[(n_seg+N)/2]. The "result" of the 
** comparison in this context is the index of the iterator that currently
** points to the smaller term/rowid combination. Iterators at EOF are
** considered to be greater than all other iterators.
**
** firsts[1] contains the index in segs[] of the iterator that points to
** the smallest key overall. firsts[0] is unused. 
**
** poslist:
**   Used by __fts5_iter_poslist() when the poslist needs to be buffered.
**   There is no way to tell if this is populated or not.
**
** colset:
**   If not NULL, points to an object containing a set of column indices.
**   Only matches that occur in one of these columns will be returned.
**   The fts5_iter_t does not own the fts5_colset_t object, and so it is not
**   freed when the iterator is closed - it is owned by the upper layer.
*/
struct fts5_iter_t {
  fts5_index_iter_t base;             /* Base class containing output vars */
  fts5_token_data_iter_t *token_data_iter;

  fts5_index_t *p_index;              /* index_t that owns this iterator */
  fts5_buffer_t poslist;             /* Buffer containing current poslist */
  fts5_colset_t *colset;            /* Restrict matches to these columns */

  /* Invoked to set output variables. */
  void (*set_outputs)(fts5_iter_t*, fts5_seg_iter_t*);

  int n_seg;                       /* Size of segs[] array */
  int rev;                       /* True to iterate in reverse order */
  u_int8_t skip_empty;                  /* True to skip deleted entries */

  int64_t i_switch_rowid;               /* Firstest rowid of other than firsts[1] */
  fts5_c_result_t *firsts;            /* Current merge state (see above) */
  fts5_seg_iter_t segs[FLEXARRAY];    /* Array of segment iterators */
};

/* Size (in bytes) of an fts5_iter_t object holding up to N segment iterators */
#define SZ_FTS5ITER(N)  (offsetof(fts5_iter_t,segs)+(N)*sizeof(fts5_seg_iter_t))

/*
** An instance of the following type is used to iterate through the contents
** of a doclist-index record.
**
** data:
**   Record containing the doclist-index data.
**
** eof:
**   Set to true once iterator has reached EOF.
**
** i_off:
**   Set to the current offset within record data.
*/
struct fts5_dlidx_lvl_t {
  fts5_data_t *data;              /* Data for current page of this level */
  int i_off;                     /* Current offset into data */
  int eof;                     /* At EOF already */
  int i_first_off;                /* Used by reverse iterators */

  /* Output variables */
  int i_leaf_pgno;                /* Page number of current leaf page */
  int64_t i_rowid;                   /* First rowid on leaf i_leaf_pgno */
};
struct fts5_dlidx_iter_t {
  int n_lvl;
  int i_segid;
  fts5_dlidx_lvl_t lvls[FLEXARRAY];
};

/* Size (in bytes) of an fts5_dlidx_iter_t object with up to N levels */
#define SZ_FTS5DLIDXITER(N) \
          (offsetof(fts5_dlidx_iter_t,lvls)+(N)*sizeof(fts5_dlidx_lvl_t))

static void __fts5_put_u16(u_int8_t *outs, u_int16_t i_val){
  outs[0] = (i_val>>8);
  outs[1] = (i_val&0xFF);
}

static u_int16_t __fts5_get_u16(const u_int8_t *ins){
  return ((u_int16_t)ins[0] << 8) + ins[1];
} 

/*
** The only argument points to a buffer at least 8 bytes in size. This
** function interprets the first 8 bytes of the buffer as a 64-bit big-endian
** unsigned integer and returns the result.
*/
static u_int64_t __fts5_get_u64(u_int8_t *a){
  return ((u_int64_t)a[0] << 56)
       + ((u_int64_t)a[1] << 48)
       + ((u_int64_t)a[2] << 40)
       + ((u_int64_t)a[3] << 32)
       + ((u_int64_t)a[4] << 24)
       + ((u_int64_t)a[5] << 16)
       + ((u_int64_t)a[6] << 8)
       + ((u_int64_t)a[7] << 0);
}

/*
** The only argument points to a buffer at least 4 bytes in size. This
** function interprets the first 4 bytes of the buffer as a 32-bit big-endian
** unsigned integer and returns the result.
*/
static u_int32_t __fts5_get_u32(const u_int8_t *a){
  return ((u_int32_t)a[0] << 24)
       + ((u_int32_t)a[1] << 16)
       + ((u_int32_t)a[2] << 8)
       + ((u_int32_t)a[3] << 0);
} 

/*
** Write i_val, formated as a 64-bit big-endian unsigned integer, to the
** buffer indicated by the first argument.
*/
static void __fts5_put_u64(u_int8_t *a, u_int64_t i_val){
  a[0] = ((i_val >> 56) & 0xFF);
  a[1] = ((i_val >> 48) & 0xFF);
  a[2] = ((i_val >> 40) & 0xFF);
  a[3] = ((i_val >> 32) & 0xFF);
  a[4] = ((i_val >> 24) & 0xFF);
  a[5] = ((i_val >> 16) & 0xFF);
  a[6] = ((i_val >>  8) & 0xFF);
  a[7] = ((i_val >>  0) & 0xFF);
}

/*
** Write i_val, formated as a 32-bit big-endian unsigned integer, to the
** buffer indicated by the first argument.
*/
static void __fts5_put_u32(u_int8_t *a, u_int32_t i_val){
  a[0] = ((i_val >> 24) & 0xFF);
  a[1] = ((i_val >> 16) & 0xFF);
  a[2] = ((i_val >>  8) & 0xFF);
  a[3] = ((i_val >>  0) & 0xFF);
}

/*
** Allocate and return a buffer at least n_byte bytes in size.
**
** If an OOM error is encountered, return NULL and set the error code in
** the fts5_index_t handle passed as the first argument.
*/
static void *__fts5_idx_malloc(fts5_index_t *p, int64_t n_byte){
  return __fts5_malloc_zero(&p->rc, n_byte);
}

/*
** Compare the contents of the left buffer with the right/n_right blob.
**
** Return -ve if left is smaller than right, 0 if they are equal or
** +ve if right is smaller than left. In other words:
**
**     res = *left - *right
*/
#ifdef DBSQL_DEBUG
static int __fts5_buffer_compare_blob(
  fts5_buffer_t *left,              /* Left hand side of comparison */
  const u_int8_t *right, int n_right    /* Right hand side of comparison */
){
  int n_cmp = MIN(left->n, n_right);
  int res = memcmp(left->p, right, n_cmp);
  return (res==0 ? (left->n - n_right) : res);
}
#endif

/*
** Compare the contents of the two buffers using memcmp(). If one buffer
** is a prefix of the other, it is considered the lesser.
**
** Return -ve if left is smaller than right, 0 if they are equal or
** +ve if right is smaller than left. In other words:
**
**     res = *left - *right
*/
static int __fts5_buffer_compare(fts5_buffer_t *left, fts5_buffer_t *right){
  int n_cmp, res;
  n_cmp = MIN(left->n, right->n);
  assert( n_cmp<=0 || left->p!=0 );
  assert( n_cmp<=0 || right->p!=0 );
  res = FTS5_MEMCMP(left->p, right->p, n_cmp);
  return (res==0 ? (left->n - right->n) : res);
}

static int __fts5_leaf_first_term_off(fts5_data_t *leaf){
  int ret;
  FTS5_GET_VARINT32(&leaf->p[leaf->sz_leaf], ret);
  return ret;
}

/*
** Close the read-only blob handle, if it is open.
*/
static void __fts5_index_close_reader_local(fts5_index_t *p){
  if( p->reader ){
    int rc;
    dbsql_blob_t *reader = p->reader;
    p->reader = 0;
    rc = dbsql_blob_close(reader);
    if( p->rc==DBSQL_OK ) p->rc = rc;
  }
}

/*
** Retrieve a record from the %_data table.
**
** If an error occurs, NULL is returned and an error left in the 
** fts5_index_t object.
*/
static fts5_data_t *__fts5_data_read(fts5_index_t *p, int64_t i_rowid){
  fts5_data_t *ret = 0;
  if( p->rc==DBSQL_OK ){
    int rc = DBSQL_OK;

    if( p->reader ){
      /* This call may return DBSQL_ABORT if there has been a savepoint
      ** rollback since it was last used. In this case a new blob handle
      ** is required.  */
      dbsql_blob_t *blob = p->reader;
      p->reader = 0;
      rc = dbsql_blob_reopen(blob, i_rowid);
      assert( p->reader==0 );
      p->reader = blob;
      if( rc!=DBSQL_OK ){
        __fts5_index_close_reader_local(p);
      }
      if( rc==DBSQL_ABORT ) rc = DBSQL_OK;
    }

    /* If the blob handle is not open at this point, open it and seek 
    ** to the requested entry.  */
    if( p->reader==0 && rc==DBSQL_OK ){
      fts5_config_t *config = p->config;
      rc = dbsql_blob_open(config->db, 
          config->z_db, p->data_tbl, "block", i_rowid, 0, &p->reader
      );
    }

    /* If either of the dbsql_blob_open() or dbsql_blob_reopen() calls
    ** above returned DBSQL_ERROR, return DBSQL_CORRUPT_VTAB instead.
    ** All the reasons those functions might return DBSQL_ERROR - missing
    ** table, missing row, non-blob/text in block column - indicate 
    ** backing store corruption.  */
    if( rc==DBSQL_ERROR ) rc = FTS5_CORRUPT_ROWID(p, i_rowid);

    if( rc==DBSQL_OK ){
      u_int8_t *outs = 0;               /* Read blob data into this buffer */
      int64_t n_byte = dbsql_blob_bytes(p->reader);
      int64_t sz_data = (sizeof(fts5_data_t) + 7) & ~7;
      int64_t n_alloc = sz_data + n_byte + FTS5_DATA_PADDING;
      ret = (fts5_data_t*)dbsql_malloc64(n_alloc);
      if( ret ){
        ret->nn = n_byte;
        ret->sz_leaf = 0;
        outs = ret->p = (u_int8_t*)ret + sz_data;
      }else{
        rc = DBSQL_NOMEM;
      }

      if( rc==DBSQL_OK ){
        rc = dbsql_blob_read(p->reader, outs, n_byte, 0);
      }
      if( rc!=DBSQL_OK ){
        dbsql_free(ret);
        ret = 0;
      }else{
        ret->p[n_byte] = 0x00;
        ret->p[n_byte+1] = 0x00;
      }
    }
    p->rc = rc;
    p->n_read++;
  }

  assert( (ret==0)==(p->rc!=DBSQL_OK) );
  assert( ret==0 || EIGHT_BYTE_ALIGNMENT( ret->p ) );
  return ret;
}


/*
** Release a reference to data record returned by an earlier call to
** __fts5_data_read().
*/
static void __fts5_data_release(fts5_data_t *data){
  dbsql_free(data);
}

/*
** Read a leaf-page record. This is similar to __fts5_data_read(), except that
** it fills in the fts5_data_t.sz_leaf value before returning.
*/
static fts5_data_t *__fts5_leaf_read(fts5_index_t *p, int64_t i_rowid){
  fts5_data_t *ret = __fts5_data_read(p, i_rowid);
  if( ret ){
    assert( ret->sz_leaf==0 );
    if( ret->nn>=4 ){
      ret->sz_leaf = __fts5_get_u16(&ret->p[2]);
    }
    if( ret->sz_leaf<4 || ret->sz_leaf>ret->nn ){
      FTS5_CORRUPT_ROWID(p, i_rowid);
      __fts5_data_release(ret);
      ret = 0;
    }
  }
  return ret;
}

static int __fts5_index_prepare_stmt(
  fts5_index_t *p,
  dbsql_stmt_t **stmt,
  char *sql
){
  if( p->rc==DBSQL_OK ){
    if( sql ){
      int rc = dbsql_prepare_v3(p->config->db, sql, -1,
          DBSQL_PREPARE_PERSISTENT|DBSQL_PREPARE_NO_VTAB,
          stmt, 0);
      /* If this prepare() call fails with DBSQL_ERROR, then one of the
      ** %_idx or %_data tables has been removed or modified. Call this
      ** corruption.  */
      p->rc = (rc==DBSQL_ERROR ? DBSQL_CORRUPT : rc);
    }else{
      p->rc = DBSQL_NOMEM;
    }
  }
  dbsql_free(sql);
  return p->rc;
}


/*
** INSERT OR REPLACE a record into the %_data table.
*/
static void __fts5_data_write(fts5_index_t *p, int64_t i_rowid, const u_int8_t *data, int n_data){
  if( p->rc!=DBSQL_OK ) return;

  if( p->writer==0 ){
    fts5_config_t *config = p->config;
    __fts5_index_prepare_stmt(p, &p->writer, dbsql_mprintf(
          "REPLACE INTO '%q'.'%q_data'(id, block) VALUES(?,?)", 
          config->z_db, config->name
    ));
    if( p->rc ) return;
  }

  dbsql_bind_int64(p->writer, 1, i_rowid);
  dbsql_bind_blob(p->writer, 2, data, n_data, DBSQL_STATIC);
  dbsql_step(p->writer);
  p->rc = dbsql_reset(p->writer);
  dbsql_bind_null(p->writer, 2);
}

/*
** Execute the following SQL:
**
**     DELETE FROM %_data WHERE id BETWEEN $i_first AND $i_last
*/
static void __fts5_data_delete(fts5_index_t *p, int64_t i_first, int64_t i_last){
  if( p->rc!=DBSQL_OK ) return;

  if( p->deleter==0 ){
    fts5_config_t *config = p->config;
    char *sql = dbsql_mprintf(
        "DELETE FROM '%q'.'%q_data' WHERE id>=? AND id<=?", 
          config->z_db, config->name
    );
    if( __fts5_index_prepare_stmt(p, &p->deleter, sql) ) return;
  }

  dbsql_bind_int64(p->deleter, 1, i_first);
  dbsql_bind_int64(p->deleter, 2, i_last);
  dbsql_step(p->deleter);
  p->rc = dbsql_reset(p->deleter);
}

/*
** Remove all records associated with segment i_segid.
*/
static void __fts5_data_remove_segment(fts5_index_t *p, fts5_structure_segment_t *seg){
  int i_segid = seg->i_segid;
  int64_t i_first = FTS5_SEGMENT_ROWID(i_segid, 0);
  int64_t i_last = FTS5_SEGMENT_ROWID(i_segid+1, 0)-1;
  __fts5_data_delete(p, i_first, i_last);

  if( seg->n_pg_tombstone ){
    int64_t i_tomb1 = FTS5_TOMBSTONE_ROWID(i_segid, 0);
    int64_t i_tomb2 = FTS5_TOMBSTONE_ROWID(i_segid, seg->n_pg_tombstone-1);
    __fts5_data_delete(p, i_tomb1, i_tomb2);
  }
  if( p->idx_deleter==0 ){
    fts5_config_t *config = p->config;
    __fts5_index_prepare_stmt(p, &p->idx_deleter, dbsql_mprintf(
          "DELETE FROM '%q'.'%q_idx' WHERE segid=?",
          config->z_db, config->name
    ));
  }
  if( p->rc==DBSQL_OK ){
    dbsql_bind_int(p->idx_deleter, 1, i_segid);
    dbsql_step(p->idx_deleter);
    p->rc = dbsql_reset(p->idx_deleter);
  }
}

/*
** Release a reference to an fts5_structure_t object returned by an earlier 
** call to __fts5_structure_read() or __fts5_structure_decode().
*/
static void __fts5_structure_release_local(fts5_structure_t *p_struct){
  if( p_struct && 0>=(--p_struct->n_ref) ){
    int i;
    assert( p_struct->n_ref==0 );
    for(i=0; i<p_struct->n_level; i++){
      dbsql_free(p_struct->levels[i].segs);
    }
    dbsql_free(p_struct);
  }
}

static void __fts5_structure_ref_local(fts5_structure_t *p_struct){
  p_struct->n_ref++;
}

void *__fts5_structure_ref(fts5_index_t *p){
  __fts5_structure_ref_local(p->p_struct);
  return (void*)p->p_struct;
}
void __fts5_structure_release(void *p){
  if( p ){
    __fts5_structure_release_local((fts5_structure_t*)p);
  }
}
int __fts5_structure_test(fts5_index_t *p, void *p_struct){
  if( p->p_struct!=(fts5_structure_t*)p_struct ){
    return DBSQL_ABORT;
  }
  return DBSQL_OK;
}

/*
** Ensure that structure object (*pp) is writable.
**
** This function is a no-op if (*rc) is not DBSQL_OK when it is called. If
** an error occurs, (*rc) is set to an dbsql error code before returning.
*/
static void __fts5_structure_make_writable(int *rc, fts5_structure_t **pp){
  fts5_structure_t *p = *pp;
  if( *rc==DBSQL_OK && p->n_ref>1 ){
    int64_t n_byte = SZ_FTS5STRUCTURE(p->n_level);
    fts5_structure_t *p_new;
    p_new = (fts5_structure_t*)__fts5_malloc_zero(rc, n_byte);
    if( p_new ){
      int i;
      memcpy(p_new, p, n_byte);
      for(i=0; i<p->n_level; i++) p_new->levels[i].segs = 0;
      for(i=0; i<p->n_level; i++){
        fts5_structure_level_t *lvl = &p_new->levels[i];
        n_byte = sizeof(fts5_structure_segment_t) * p_new->levels[i].n_seg;
        lvl->segs = (fts5_structure_segment_t*)__fts5_malloc_zero(rc, n_byte);
        if( lvl->segs==0 ){
          for(i=0; i<p->n_level; i++){
            dbsql_free(p_new->levels[i].segs);
          }
          dbsql_free(p_new);
          return;
        }
        memcpy(lvl->segs, p->levels[i].segs, n_byte);
      }
      p->n_ref--;
      p_new->n_ref = 1;
    }
    *pp = p_new;
  }
}

/*
** Deserialize and return the structure record currently stored in serialized
** form within buffer data/n_data.
**
** The fts5_structure_t.levels[] and each fts5_structure_level_t.segs[] array
** are over-allocated by one slot. This allows the structure contents
** to be more easily edited.
**
** If an error occurs, *out is set to NULL and an dbsql error code
** returned. Otherwise, *out is set to point to the new object and
** DBSQL_OK returned.
*/
static int __fts5_structure_decode(
  const u_int8_t *data,                /* Buffer containing serialized structure */
  int n_data,                      /* Size of buffer data in bytes */
  int *pi_cookie,                  /* Configuration cookie value */
  fts5_structure_t **out           /* OUT: Deserialized object */
){
  int rc = DBSQL_OK;
  int i = 0;
  int i_lvl;
  int n_level = 0;
  int n_segment = 0;
  int64_t n_byte;            /* Bytes of space to allocate at ret */
  fts5_structure_t *ret = 0;        /* Structure object to return */
  int structure_v2 = 0;           /* True for FTS5_STRUCTURE_V2 */
  u_int64_t n_origin_cntr = 0;            /* Largest origin value seen so far */

  /* Grab the cookie value */
  if( pi_cookie ) *pi_cookie = __fts5_get32(data);
  i = 4;

  /* Check if this is a V2 structure record. Set structure_v2 if it is. */
  if( 0==memcmp(&data[i], FTS5_STRUCTURE_V2, 4) ){
    i += 4;
    structure_v2 = 1;
  }

  /* Read the total number of levels and segments from the start of the
  ** structure record.  */
  i += FTS5_GET_VARINT32(&data[i], n_level);
  i += FTS5_GET_VARINT32(&data[i], n_segment);
  if( n_level>FTS5_MAX_SEGMENT   || n_level<0
   || n_segment>FTS5_MAX_SEGMENT || n_segment<0
  ){
    return FTS5_CORRUPT;
  }
  n_byte = SZ_FTS5STRUCTURE(n_level);
  ret = (fts5_structure_t*)__fts5_malloc_zero(&rc, n_byte);

  if( ret ){
    ret->n_ref = 1;
    ret->n_level = n_level;
    ret->n_segment = n_segment;
    i += __fts5_get_varint(&data[i], &ret->n_write_counter);

    for(i_lvl=0; rc==DBSQL_OK && i_lvl<n_level; i_lvl++){
      fts5_structure_level_t *lvl = &ret->levels[i_lvl];
      int n_total = 0;
      int i_seg;

      if( i>=n_data ){
        rc = FTS5_CORRUPT;
      }else{
        i += FTS5_GET_VARINT32(&data[i], lvl->n_merge);
        i += FTS5_GET_VARINT32(&data[i], n_total);
        if( n_total<lvl->n_merge ) rc = FTS5_CORRUPT;
        lvl->segs = (fts5_structure_segment_t*)__fts5_malloc_zero(&rc, 
            (int64_t)n_total * sizeof(fts5_structure_segment_t)
        );
        n_segment -= n_total;
      }

      if( rc==DBSQL_OK ){
        lvl->n_seg = n_total;
        for(i_seg=0; i_seg<n_total; i_seg++){
          fts5_structure_segment_t *seg = &lvl->segs[i_seg];
          if( i>=n_data ){
            rc = FTS5_CORRUPT;
            break;
          }
          assert( seg!=0 );
          i += FTS5_GET_VARINT32(&data[i], seg->i_segid);
          i += FTS5_GET_VARINT32(&data[i], seg->pgno_first);
          i += FTS5_GET_VARINT32(&data[i], seg->pgno_last);
          if( structure_v2 ){
            i += __fts5_get_varint(&data[i], &seg->i_origin1);
            i += __fts5_get_varint(&data[i], &seg->i_origin2);
            i += FTS5_GET_VARINT32(&data[i], seg->n_pg_tombstone);
            i += __fts5_get_varint(&data[i], &seg->n_entry_tombstone);
            i += __fts5_get_varint(&data[i], &seg->n_entry);
            n_origin_cntr = MAX(n_origin_cntr, seg->i_origin2);
          }
          if( seg->pgno_last<seg->pgno_first ){
            rc = FTS5_CORRUPT;
            break;
          }
        }
        if( i_lvl>0 && lvl[-1].n_merge && n_total==0 ) rc = FTS5_CORRUPT;
        if( i_lvl==n_level-1 && lvl->n_merge ) rc = FTS5_CORRUPT;
      }
    }
    if( n_segment!=0 && rc==DBSQL_OK ) rc = FTS5_CORRUPT;
    if( structure_v2 ){
      ret->n_origin_cntr = n_origin_cntr+1;
    }

    if( rc!=DBSQL_OK ){
      __fts5_structure_release_local(ret);
      ret = 0;
    }
  }

  *out = ret;
  return rc;
}

/*
** Add a level to the fts5_structure_t.levels[] array of structure object
** (*pp_struct).
*/
static void __fts5_structure_add_level(int *rc, fts5_structure_t **pp_struct){
  __fts5_structure_make_writable(rc, pp_struct);
  assert( (pp_struct!=0 && (*pp_struct)!=0) || (*rc)!=DBSQL_OK );
  if( *rc==DBSQL_OK ){
    fts5_structure_t *p_struct = *pp_struct;
    int n_level = p_struct->n_level;
    int64_t n_byte = SZ_FTS5STRUCTURE(n_level+2);

    p_struct = dbsql_realloc64(p_struct, n_byte);
    if( p_struct ){
      memset(&p_struct->levels[n_level], 0, sizeof(fts5_structure_level_t));
      p_struct->n_level++;
      *pp_struct = p_struct;
    }else{
      *rc = DBSQL_NOMEM;
    }
  }
}

/*
** Extend level i_lvl so that there is room for at least n_extra more
** segments.
*/
static void __fts5_structure_extend_level(
  int *rc, 
  fts5_structure_t *p_struct, 
  int i_lvl, 
  int n_extra, 
  int insert
){
  if( *rc==DBSQL_OK ){
    fts5_structure_level_t *lvl = &p_struct->levels[i_lvl];
    fts5_structure_segment_t *news;
    int64_t n_byte;

    n_byte = (lvl->n_seg + n_extra) * sizeof(fts5_structure_segment_t);
    news = dbsql_realloc64(lvl->segs, n_byte);
    if( news ){
      if( insert==0 ){
        memset(&news[lvl->n_seg], 0, sizeof(fts5_structure_segment_t) * n_extra);
      }else{
        int n_move = lvl->n_seg * sizeof(fts5_structure_segment_t);
        memmove(&news[n_extra], news, n_move);
        memset(news, 0, sizeof(fts5_structure_segment_t) * n_extra);
      }
      lvl->segs = news;
    }else{
      *rc = DBSQL_NOMEM;
    }
  }
}

static fts5_structure_t *__fts5_structure_read_uncached(fts5_index_t *p){
  fts5_structure_t *ret = 0;
  fts5_config_t *config = p->config;
  int i_cookie;                    /* Configuration cookie */
  fts5_data_t *data;

  data = __fts5_data_read(p, FTS5_STRUCTURE_ROWID);
  if( p->rc==DBSQL_OK ){
    /* TODO: Do we need this if the leaf-index is appended? Probably... */
    memset(&data->p[data->nn], 0, FTS5_DATA_PADDING);
    p->rc = __fts5_structure_decode(data->p, data->nn, &i_cookie, &ret);
    if( p->rc==DBSQL_OK ){
      if( (config->pgsz==0 || config->i_cookie!=i_cookie) ){
        p->rc = __fts5_config_load(config, i_cookie);
      }
    }else if( p->rc==DBSQL_CORRUPT_VTAB ){
      __fts5_config_errmsg(p->config, 
          "fts5: corrupt structure record for table \"%s\"", p->config->name
      );
    }
    __fts5_data_release(data);
    if( p->rc!=DBSQL_OK ){
      __fts5_structure_release_local(ret);
      ret = 0;
    }
  }

  return ret;
}

static int64_t __fts5_index_data_version(fts5_index_t *p){
  int64_t i_version = 0;

  if( p->rc==DBSQL_OK ){
    if( p->data_version==0 ){
      p->rc = __fts5_index_prepare_stmt(p, &p->data_version, 
          dbsql_mprintf("PRAGMA %Q.data_version", p->config->z_db)
          );
      if( p->rc ) return 0;
    }

    if( DBSQL_ROW==dbsql_step(p->data_version) ){
      i_version = dbsql_column_int64(p->data_version, 0);
    }
    p->rc = dbsql_reset(p->data_version);
  }

  return i_version;
}

/*
** Read, deserialize and return the structure record.
**
** The fts5_structure_t.levels[] and each fts5_structure_level_t.segs[] array
** are over-allocated as described for function __fts5_structure_decode() 
** above.
**
** If an error occurs, NULL is returned and an error code left in the
** fts5_index_t handle. If an error has already occurred when this function
** is called, it is a no-op.
*/
static fts5_structure_t *__fts5_structure_read(fts5_index_t *p){

  if( p->p_struct==0 ){
    p->i_struct_version = __fts5_index_data_version(p);
    if( p->rc==DBSQL_OK ){
      p->p_struct = __fts5_structure_read_uncached(p);
    }
  }

#if 0
  else{
    fts5_structure_t *test = __fts5_structure_read_uncached(p);
    if( test ){
      int i, j;
      assert_nc( p->p_struct->n_segment==test->n_segment );
      assert_nc( p->p_struct->n_level==test->n_level );
      for(i=0; i<test->n_level; i++){
        assert_nc( p->p_struct->levels[i].n_merge==test->levels[i].n_merge );
        assert_nc( p->p_struct->levels[i].n_seg==test->levels[i].n_seg );
        for(j=0; j<test->levels[i].n_seg; j++){
          fts5_structure_segment_t *p1 = &test->levels[i].segs[j];
          fts5_structure_segment_t *p2 = &p->p_struct->levels[i].segs[j];
          assert_nc( p1->i_segid==p2->i_segid );
          assert_nc( p1->pgno_first==p2->pgno_first );
          assert_nc( p1->pgno_last==p2->pgno_last );
        }
      }
      __fts5_structure_release_local(test);
    }
  }
#endif

  if( p->rc!=DBSQL_OK ) return 0;
  assert( p->i_struct_version!=0 );
  assert( p->p_struct!=0 );
  __fts5_structure_ref_local(p->p_struct);
  return p->p_struct;
}

static void __fts5_structure_invalidate(fts5_index_t *p){
  if( p->p_struct ){
    __fts5_structure_release_local(p->p_struct);
    p->p_struct = 0;
  }
}

/*
** Return the total number of segments in index structure p_struct. This
** function is only ever used as part of assert() conditions.
*/
#ifdef DBSQL_DEBUG
static int __fts5_structure_count_segments(fts5_structure_t *p_struct){
  int n_segment = 0;               /* Total number of segments */
  if( p_struct ){
    int i_lvl;                     /* Used to iterate through levels */
    for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
      n_segment += p_struct->levels[i_lvl].n_seg;
    }
  }

  return n_segment;
}
#endif

#define FTS5_BUFFER_SAFE_APPEND_BLOB(buf, blob, n_blob) {     \
  assert( (buf)->n_space>=((buf)->n+n_blob) );             \
  memcpy(&(buf)->p[(buf)->n], blob, n_blob);             \
  (buf)->n += n_blob;                                      \
}

#define FTS5_BUFFER_SAFE_APPEND_VARINT(buf, i_val) {                \
  (buf)->n += __fts5_put_varint(&(buf)->p[(buf)->n], (i_val));  \
  assert( (buf)->n_space>=(buf)->n );                          \
}


/*
** Serialize and store the "structure" record.
**
** If an error occurs, leave an error code in the fts5_index_t object. If an
** error has already occurred, this function is a no-op.
*/
static void __fts5_structure_write(fts5_index_t *p, fts5_structure_t *p_struct){
  if( p->rc==DBSQL_OK ){
    fts5_buffer_t buf;               /* Buffer to serialize record into */
    int i_lvl;                     /* Used to iterate through levels */
    int i_cookie;                  /* Cookie value to store */
    int n_hdr = (p_struct->n_origin_cntr>0 ? (4+4+9+9+9) : (4+9+9));

    assert( p_struct->n_segment==__fts5_structure_count_segments(p_struct) );
    memset(&buf, 0, sizeof(fts5_buffer_t));

    /* Append the current configuration cookie */
    i_cookie = p->config->i_cookie;
    if( i_cookie<0 ) i_cookie = 0;

    if( 0==__fts5_buffer_size(&p->rc, &buf, n_hdr) ){
      __fts5_put32(buf.p, i_cookie);
      buf.n = 4;
      if( p_struct->n_origin_cntr>0 ){
        FTS5_BUFFER_SAFE_APPEND_BLOB(&buf, FTS5_STRUCTURE_V2, 4);
      }
      FTS5_BUFFER_SAFE_APPEND_VARINT(&buf, p_struct->n_level);
      FTS5_BUFFER_SAFE_APPEND_VARINT(&buf, p_struct->n_segment);
      FTS5_BUFFER_SAFE_APPEND_VARINT(&buf, (int64_t)p_struct->n_write_counter);
    }

    for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
      int i_seg;                     /* Used to iterate through segments */
      fts5_structure_level_t *lvl = &p_struct->levels[i_lvl];
      FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, lvl->n_merge);
      FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, lvl->n_seg);
      assert( lvl->n_merge<=lvl->n_seg );

      for(i_seg=0; i_seg<lvl->n_seg; i_seg++){
        fts5_structure_segment_t *seg = &lvl->segs[i_seg];
        FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->i_segid);
        FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->pgno_first);
        FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->pgno_last);
        if( p_struct->n_origin_cntr>0 ){
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->i_origin1);
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->i_origin2);
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->n_pg_tombstone);
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->n_entry_tombstone);
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->n_entry);
        }
      }
    }

    __fts5_data_write(p, FTS5_STRUCTURE_ROWID, buf.p, buf.n);
    FTS5_BUFFER_FREE(&buf);
  }
}

#if 0
static void __fts5_debug_structure(int*,fts5_buffer_t*,fts5_structure_t*);
static void FTS5_PRINT_STRUCTURE(const char *caption, fts5_structure_t *p_struct){
  int rc = DBSQL_OK;
  fts5_buffer_t buf;
  memset(&buf, 0, sizeof(buf));
  __fts5_debug_structure(&rc, &buf, p_struct);
  fprintf(stdout, "%s: %s\n", caption, buf.p);
  fflush(stdout);
  FTS5_BUFFER_FREE(&buf);
}
#else
# define FTS5_PRINT_STRUCTURE(x,y)
#endif

static int __fts5_segment_size(fts5_structure_segment_t *seg){
  return 1 + seg->pgno_last - seg->pgno_first;
}

/*
** Return a copy of index structure p_struct. Except, promote as many 
** segments as possible to level i_promote. If an OOM occurs, NULL is 
** returned.
*/
static void __fts5_structure_promote_to(
  fts5_index_t *p,
  int i_promote,
  int sz_promote,
  fts5_structure_t *p_struct
){
  int il, is;
  fts5_structure_level_t *out = &p_struct->levels[i_promote];

  if( out->n_merge==0 ){
    for(il=i_promote+1; il<p_struct->n_level; il++){
      fts5_structure_level_t *lvl = &p_struct->levels[il];
      if( lvl->n_merge ) return;
      for(is=lvl->n_seg-1; is>=0; is--){
        int sz = __fts5_segment_size(&lvl->segs[is]);
        if( sz>sz_promote ) return;
        __fts5_structure_extend_level(&p->rc, p_struct, i_promote, 1, 1);
        if( p->rc ) return;
        memcpy(out->segs, &lvl->segs[is], sizeof(fts5_structure_segment_t));
        out->n_seg++;
        lvl->n_seg--;
      }
    }
  }
}

/*
** A new segment has just been written to level i_lvl of index structure
** p_struct. This function determines if any segments should be promoted
** as a result. Segments are promoted in two scenarios:
**
**   a) If the segment just written is smaller than one or more segments
**      within the previous populated level, it is promoted to the previous
**      populated level.
**
**   b) If the segment just written is larger than the newest segment on
**      the next populated level, then that segment, and any other adjacent
**      segments that are also smaller than the one just written, are 
**      promoted. 
**
** If one or more segments are promoted, the structure object is updated
** to reflect this.
*/
static void __fts5_structure_promote(
  fts5_index_t *p,                   /* FTS5 backend object */
  int i_lvl,                       /* index_t level just updated */
  fts5_structure_t *p_struct          /* index_t structure */
){
  if( p->rc==DBSQL_OK ){
    int i_tst;
    int i_promote = -1;
    int sz_promote = 0;            /* Promote anything this size or smaller */
    fts5_structure_segment_t *seg;   /* Segment just written */
    int sz_seg;                    /* Size of segment just written */
    int n_seg = p_struct->levels[i_lvl].n_seg;

    if( n_seg==0 ) return;
    seg = &p_struct->levels[i_lvl].segs[p_struct->levels[i_lvl].n_seg-1];
    sz_seg = (1 + seg->pgno_last - seg->pgno_first);

    /* Check for condition (a) */
    for(i_tst=i_lvl-1; i_tst>=0 && p_struct->levels[i_tst].n_seg==0; i_tst--);
    if( i_tst>=0 ){
      int i;
      int sz_max = 0;
      fts5_structure_level_t *tst = &p_struct->levels[i_tst];
      assert( tst->n_merge==0 );
      for(i=0; i<tst->n_seg; i++){
        int sz = tst->segs[i].pgno_last - tst->segs[i].pgno_first + 1;
        if( sz>sz_max ) sz_max = sz;
      }
      if( sz_max>=sz_seg ){
        /* Condition (a) is true. Promote the newest segment on level 
        ** i_lvl to level i_tst.  */
        i_promote = i_tst;
        sz_promote = sz_max;
      }
    }

    /* If condition (a) is not met, assume (b) is true. __structure_promote_to()
    ** is a no-op if it is not.  */
    if( i_promote<0 ){
      i_promote = i_lvl;
      sz_promote = sz_seg;
    }
    __fts5_structure_promote_to(p, i_promote, sz_promote, p_struct);
  }
}


/*
** Advance the iterator passed as the only argument. If the end of the 
** doclist-index page is reached, return non-zero.
*/
static int __fts5_dlidx_lvl_next(fts5_dlidx_lvl_t *lvl){
  fts5_data_t *data = lvl->data;

  if( lvl->i_off==0 ){
    assert( lvl->eof==0 );
    lvl->i_off = 1;
    lvl->i_off += FTS5_GET_VARINT32(&data->p[1], lvl->i_leaf_pgno);
    lvl->i_off += __fts5_get_varint(&data->p[lvl->i_off], (u_int64_t*)&lvl->i_rowid);
    lvl->i_first_off = lvl->i_off;
  }else{
    int i_off;
    for(i_off=lvl->i_off; i_off<data->nn; i_off++){
      if( data->p[i_off] ) break; 
    }

    if( i_off<data->nn ){
      u_int64_t i_val;
      lvl->i_leaf_pgno += (i_off - lvl->i_off) + 1;
      i_off += __fts5_get_varint(&data->p[i_off], &i_val);
      lvl->i_rowid += i_val;
      lvl->i_off = i_off;
    }else{
      lvl->eof = 1;
    }
  }

  return lvl->eof;
}

/*
** Advance the iterator passed as the only argument.
*/
static int __fts5_dlidx_iter_next_r(fts5_index_t *p, fts5_dlidx_iter_t *iter, int i_lvl){
  fts5_dlidx_lvl_t *lvl = &iter->lvls[i_lvl];

  assert( i_lvl<iter->n_lvl );
  if( __fts5_dlidx_lvl_next(lvl) ){
    if( (i_lvl+1) < iter->n_lvl ){
      __fts5_dlidx_iter_next_r(p, iter, i_lvl+1);
      if( lvl[1].eof==0 ){
        __fts5_data_release(lvl->data);
        memset(lvl, 0, sizeof(fts5_dlidx_lvl_t));
        lvl->data = __fts5_data_read(p, 
            FTS5_DLIDX_ROWID(iter->i_segid, i_lvl, lvl[1].i_leaf_pgno)
        );
        if( lvl->data ) __fts5_dlidx_lvl_next(lvl);
      }
    }
  }

  return iter->lvls[0].eof;
}
static int __fts5_dlidx_iter_next(fts5_index_t *p, fts5_dlidx_iter_t *iter){
  return __fts5_dlidx_iter_next_r(p, iter, 0);
}

/*
** The iterator passed as the first argument has the following fields set
** as follows. This function sets up the rest of the iterator so that it
** points to the first rowid in the doclist-index.
**
**   data:
**     pointer to doclist-index record, 
**
** When this function is called iter->i_leaf_pgno is the page number the
** doclist is associated with (the one featuring the term).
*/
static int __fts5_dlidx_iter_first(fts5_dlidx_iter_t *iter){
  int i;
  for(i=0; i<iter->n_lvl; i++){
    __fts5_dlidx_lvl_next(&iter->lvls[i]);
  }
  return iter->lvls[0].eof;
}


static int __fts5_dlidx_iter_eof(fts5_index_t *p, fts5_dlidx_iter_t *iter){
  return p->rc!=DBSQL_OK || iter->lvls[0].eof;
}

static void __fts5_dlidx_iter_last(fts5_index_t *p, fts5_dlidx_iter_t *iter){
  int i;

  /* Advance each level to the last entry on the last page */
  for(i=iter->n_lvl-1; p->rc==DBSQL_OK && i>=0; i--){
    fts5_dlidx_lvl_t *lvl = &iter->lvls[i];
    while( __fts5_dlidx_lvl_next(lvl)==0 );
    lvl->eof = 0;

    if( i>0 ){
      fts5_dlidx_lvl_t *child = &lvl[-1];
      __fts5_data_release(child->data);
      memset(child, 0, sizeof(fts5_dlidx_lvl_t));
      child->data = __fts5_data_read(p, 
          FTS5_DLIDX_ROWID(iter->i_segid, i-1, lvl->i_leaf_pgno)
      );
    }
  }
}

/*
** Move the iterator passed as the only argument to the previous entry.
*/
static int __fts5_dlidx_lvl_prev(fts5_dlidx_lvl_t *lvl){
  int i_off = lvl->i_off;

  assert( lvl->eof==0 );
  if( i_off<=lvl->i_first_off ){
    lvl->eof = 1;
  }else{
    u_int8_t *a = lvl->data->p;

    lvl->i_off = 0;
    __fts5_dlidx_lvl_next(lvl);
    while( 1 ){
      int n_zero = 0;
      int ii = lvl->i_off;
      u_int64_t delta = 0;

      while( a[ii]==0 ){
        n_zero++;
        ii++;
      }
      ii += __fts5_get_varint(&a[ii], &delta);

      if( ii>=i_off ) break;
      lvl->i_leaf_pgno += n_zero+1;
      lvl->i_rowid += delta;
      lvl->i_off = ii;
    }
  }

  return lvl->eof;
}

static int __fts5_dlidx_iter_prev_r(fts5_index_t *p, fts5_dlidx_iter_t *iter, int i_lvl){
  fts5_dlidx_lvl_t *lvl = &iter->lvls[i_lvl];

  assert( i_lvl<iter->n_lvl );
  if( __fts5_dlidx_lvl_prev(lvl) ){
    if( (i_lvl+1) < iter->n_lvl ){
      __fts5_dlidx_iter_prev_r(p, iter, i_lvl+1);
      if( lvl[1].eof==0 ){
        __fts5_data_release(lvl->data);
        memset(lvl, 0, sizeof(fts5_dlidx_lvl_t));
        lvl->data = __fts5_data_read(p, 
            FTS5_DLIDX_ROWID(iter->i_segid, i_lvl, lvl[1].i_leaf_pgno)
        );
        if( lvl->data ){
          while( __fts5_dlidx_lvl_next(lvl)==0 );
          lvl->eof = 0;
        }
      }
    }
  }

  return iter->lvls[0].eof;
}
static int __fts5_dlidx_iter_prev(fts5_index_t *p, fts5_dlidx_iter_t *iter){
  return __fts5_dlidx_iter_prev_r(p, iter, 0);
}

/*
** Free a doclist-index iterator object allocated by __fts5_dlidx_iter_init().
*/
static void __fts5_dlidx_iter_free(fts5_dlidx_iter_t *iter){
  if( iter ){
    int i;
    for(i=0; i<iter->n_lvl; i++){
      __fts5_data_release(iter->lvls[i].data);
    }
    dbsql_free(iter);
  }
}

static fts5_dlidx_iter_t *__fts5_dlidx_iter_init(
  fts5_index_t *p,                   /* Fts5 Backend to iterate within */
  int rev,                       /* True for ORDER BY ASC */
  int i_segid,                     /* Segment id */
  int i_leaf_pg                     /* Leaf page number to load dlidx for */
){
  fts5_dlidx_iter_t *iter = 0;
  int i;
  int done = 0;

  for(i=0; p->rc==DBSQL_OK && done==0; i++){
    int64_t n_byte = SZ_FTS5DLIDXITER(i+1);
    fts5_dlidx_iter_t *p_new;

    p_new = (fts5_dlidx_iter_t*)dbsql_realloc64(iter, n_byte);
    if( p_new==0 ){
      p->rc = DBSQL_NOMEM;
    }else{
      int64_t i_rowid = FTS5_DLIDX_ROWID(i_segid, i, i_leaf_pg);
      fts5_dlidx_lvl_t *lvl = &p_new->lvls[i];
      iter = p_new;
      memset(lvl, 0, sizeof(fts5_dlidx_lvl_t));
      lvl->data = __fts5_data_read(p, i_rowid);
      if( lvl->data && (lvl->data->p[0] & 0x0001)==0 ){
        done = 1;
      }
      iter->n_lvl = i+1;
    }
  }

  if( p->rc==DBSQL_OK ){
    iter->i_segid = i_segid;
    if( rev==0 ){
      __fts5_dlidx_iter_first(iter);
    }else{
      __fts5_dlidx_iter_last(p, iter);
    }
  }

  if( p->rc!=DBSQL_OK ){
    __fts5_dlidx_iter_free(iter);
    iter = 0;
  }

  return iter;
}

static int64_t __fts5_dlidx_iter_rowid(fts5_dlidx_iter_t *iter){
  return iter->lvls[0].i_rowid;
}
static int __fts5_dlidx_iter_pgno(fts5_dlidx_iter_t *iter){
  return iter->lvls[0].i_leaf_pgno;
}

/*
** Load the next leaf page into the segment iterator.
*/
static void __fts5_seg_iter_next_page(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter              /* Iterator to advance to next page */
){
  fts5_data_t *leaf;
  fts5_structure_segment_t *seg = iter->seg;
  __fts5_data_release(iter->leaf);
  iter->i_leaf_pgno++;
  if( iter->next_leaf ){
    iter->leaf = iter->next_leaf;
    iter->next_leaf = 0;
  }else if( iter->i_leaf_pgno<=seg->pgno_last ){
    iter->leaf = __fts5_leaf_read(p, 
        FTS5_SEGMENT_ROWID(seg->i_segid, iter->i_leaf_pgno)
    );
  }else{
    iter->leaf = 0;
  }
  leaf = iter->leaf;

  if( leaf ){
    iter->i_pgidx_off = leaf->sz_leaf;
    if( FTS5_LEAF_IS_TERMLESS(leaf) ){
      iter->i_endof_doclist = leaf->nn+1;
    }else{
      iter->i_pgidx_off += FTS5_GET_VARINT32(&leaf->p[iter->i_pgidx_off],
          iter->i_endof_doclist
      );
    }
  }
}

/*
** Argument p points to a buffer containing a varint to be interpreted as a
** position list size field. Read the varint and return the number of bytes
** read. Before returning, set *pn_sz to the number of bytes in the position
** list, and *pb_del to true if the delete flag is set, or false otherwise.
*/
static int __fts5_get_poslist_size(const u_int8_t *p, int *pn_sz, int *pb_del){
  int n_sz;
  int n = 0;
  FTS5_FAST_GET_VARINT32(p, n, n_sz);
  assert_nc( n_sz>=0 );
  *pn_sz = n_sz/2;
  *pb_del = n_sz & 0x0001;
  return n;
}

/*
** fts5_seg_iter_t.i_leaf_offset currently points to the first byte of a
** position-list size field. Read the value of the field and store it
** in the following variables:
**
**   fts5_seg_iter_t.n_pos
**   fts5_seg_iter_t.del
**
** Leave fts5_seg_iter_t.i_leaf_offset pointing to the first byte of the 
** position list content (if any).
*/
static void __fts5_seg_iter_load_n_pos(fts5_index_t *p, fts5_seg_iter_t *iter){
  if( p->rc==DBSQL_OK ){
    int i_off = iter->i_leaf_offset;  /* Offset to read at */
    ASSERT_SZLEAF_OK(iter->leaf);
    if( p->config->detail==FTS5_DETAIL_NONE ){
      int i_eod = MIN(iter->i_endof_doclist, iter->leaf->sz_leaf);
      iter->del = 0;
      iter->n_pos = 1;
      if( i_off<i_eod && iter->leaf->p[i_off]==0 ){
        iter->del = 1;
        i_off++;
        if( i_off<i_eod && iter->leaf->p[i_off]==0 ){
          iter->n_pos = 1;
          i_off++;
        }else{
          iter->n_pos = 0;
        }
      }
    }else{
      int n_sz;
      FTS5_FAST_GET_VARINT32(iter->leaf->p, i_off, n_sz);
      iter->del = (n_sz & 0x0001);
      iter->n_pos = n_sz>>1;
      assert_nc( iter->n_pos>=0 );
    }
    iter->i_leaf_offset = i_off;
  }
}

static void __fts5_seg_iter_load_rowid(fts5_index_t *p, fts5_seg_iter_t *iter){
  u_int8_t *a = iter->leaf->p;        /* Buffer to read data from */
  int64_t i_off = iter->i_leaf_offset;

  ASSERT_SZLEAF_OK(iter->leaf);
  while( i_off>=iter->leaf->sz_leaf ){
    __fts5_seg_iter_next_page(p, iter);
    if( iter->leaf==0 ){
      if( p->rc==DBSQL_OK ) FTS5_CORRUPT_ITER(p, iter);
      return;
    }
    i_off = 4;
    a = iter->leaf->p;
  }
  i_off += __fts5_get_varint(&a[i_off], (u_int64_t*)&iter->i_rowid);
  iter->i_leaf_offset = i_off;
}

/*
** fts5_seg_iter_t.i_leaf_offset currently points to the first byte of the 
** "n_suffix" field of a term. Function parameter n_keep contains the value
** of the "n_prefix" field (if there was one - it is passed 0 if this is
** the first term in the segment).
**
** This function populates:
**
**   fts5_seg_iter_t.term
**   fts5_seg_iter_t.rowid
**
** accordingly and leaves (fts5_seg_iter_t.i_leaf_offset) set to the content of
** the first position list. The position list belonging to document 
** (fts5_seg_iter_t.i_rowid).
*/
static void __fts5_seg_iter_load_term(fts5_index_t *p, fts5_seg_iter_t *iter, int n_keep){
  u_int8_t *a = iter->leaf->p;        /* Buffer to read data from */
  int64_t i_off = iter->i_leaf_offset;  /* Offset to read at */
  int n_new;                       /* Bytes of new data */

  i_off += FTS5_GET_VARINT32(&a[i_off], n_new);
  if( i_off+n_new>iter->leaf->sz_leaf || n_keep>iter->term.n || n_new==0 ){
    FTS5_CORRUPT_ITER(p, iter);
    return;
  }
  iter->term.n = n_keep;
  FTS5_BUFFER_APPEND_BLOB(&p->rc, &iter->term, n_new, &a[i_off]);
  assert( iter->term.n<=iter->term.n_space );
  i_off += n_new;
  iter->i_term_leaf_offset = i_off;
  iter->i_term_leaf_pgno = iter->i_leaf_pgno;
  iter->i_leaf_offset = i_off;

  if( iter->i_pgidx_off>=iter->leaf->nn ){
    iter->i_endof_doclist = iter->leaf->nn+1;
  }else{
    int n_extra;
    iter->i_pgidx_off += FTS5_GET_VARINT32(&a[iter->i_pgidx_off], n_extra);
    iter->i_endof_doclist += n_extra;
  }

  __fts5_seg_iter_load_rowid(p, iter);
}

static void __fts5_seg_iter_next(fts5_index_t*, fts5_seg_iter_t*, int*);
static void __fts5_seg_iter_next_reverse(fts5_index_t*, fts5_seg_iter_t*, int*);
static void __fts5_seg_iter_next_none(fts5_index_t*, fts5_seg_iter_t*, int*);

static void __fts5_seg_iter_set_next(fts5_index_t *p, fts5_seg_iter_t *iter){
  if( iter->flags & FTS5_SEGITER_REVERSE ){
    iter->next = __fts5_seg_iter_next_reverse;
  }else if( p->config->detail==FTS5_DETAIL_NONE ){
    iter->next = __fts5_seg_iter_next_none;
  }else{
    iter->next = __fts5_seg_iter_next;
  }
}

/*
** Allocate a tombstone hash page array object (iter->tomb_array) for 
** the iterator passed as the second argument. If an OOM error occurs, 
** leave an error in the fts5_index_t object.
*/
static void __fts5_seg_iter_alloc_tombstone(fts5_index_t *p, fts5_seg_iter_t *iter){
  const int64_t n_tomb = (int64_t)iter->seg->n_pg_tombstone;
  if( n_tomb>0 ){
    int64_t n_byte = SZ_FTS5TOMBSTONEARRAY(n_tomb+1);
    fts5_tombstone_array_t *p_new;
    p_new = (fts5_tombstone_array_t*)__fts5_malloc_zero(&p->rc, n_byte);
    if( p_new ){
      p_new->n_tombstone = n_tomb;
      p_new->n_ref = 1;
      iter->tomb_array = p_new;
    }
  }
}

/*
** Initialize the iterator object iter to iterate through the entries in
** segment seg. The iterator is left pointing to the first entry when 
** this function returns.
**
** If an error occurs, fts5_index_t.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void __fts5_seg_iter_init(
  fts5_index_t *p,                   /* FTS index object */
  fts5_structure_segment_t *seg,     /* Description of segment */
  fts5_seg_iter_t *iter              /* Object to populate */
){
  if( seg->pgno_first==0 ){
    /* This happens if the segment is being used as an input to an incremental
    ** merge and all data has already been "trimmed". See function
    ** __fts5_trim_segments() for details. In this case leave the iterator empty.
    ** The caller will see the (iter->leaf==0) and assume the iterator is
    ** at EOF already. */
    assert( iter->leaf==0 );
    return;
  }

  if( p->rc==DBSQL_OK ){
    memset(iter, 0, sizeof(*iter));
    __fts5_seg_iter_set_next(p, iter);
    iter->seg = seg;
    iter->i_leaf_pgno = seg->pgno_first-1;
    do {
      __fts5_seg_iter_next_page(p, iter);
    }while( p->rc==DBSQL_OK && iter->leaf && iter->leaf->nn==4 );
  }

  if( p->rc==DBSQL_OK && iter->leaf ){
    iter->i_leaf_offset = 4;
    assert( iter->leaf!=0 );
    assert_nc( iter->leaf->nn>4 );
    assert_nc( __fts5_leaf_first_term_off(iter->leaf)==4 );
    iter->i_pgidx_off = iter->leaf->sz_leaf+1;
    __fts5_seg_iter_load_term(p, iter, 0);
    __fts5_seg_iter_load_n_pos(p, iter);
    __fts5_seg_iter_alloc_tombstone(p, iter);
  }
}

/*
** This function is only ever called on iterators created by calls to
** __fts5_index_query() with the FTS5INDEX_QUERY_DESC flag set.
**
** The iterator is in an unusual state when this function is called: the
** fts5_seg_iter_t.i_leaf_offset variable is set to the offset of the start of
** the position-list size field for the first relevant rowid on the page.
** fts5_seg_iter_t.rowid is set, but n_pos and del are not.
**
** This function advances the iterator so that it points to the last 
** relevant rowid on the page and, if necessary, initializes the 
** rowid_offsets[] and i_rowid_offset variables. At this point the iterator
** is in its regular state - fts5_seg_iter_t.i_leaf_offset points to the first
** byte of the position list content associated with said rowid.
*/
static void __fts5_seg_iter_reverse_init_page(fts5_index_t *p, fts5_seg_iter_t *iter){
  int detail = p->config->detail;
  int n = iter->leaf->sz_leaf;
  int i = iter->i_leaf_offset;
  u_int8_t *a = iter->leaf->p;
  int i_rowid_offset = 0;

  if( n>iter->i_endof_doclist ){
    n = iter->i_endof_doclist;
  }

  ASSERT_SZLEAF_OK(iter->leaf);
  while( 1 ){
    u_int64_t i_delta = 0;

    if( i>=n ) break;
    if( detail==FTS5_DETAIL_NONE ){
      /* todo */
      if( i<n && a[i]==0 ){
        i++;
        if( i<n && a[i]==0 ) i++;
      }
    }else{
      int n_pos;
      int dummy;
      i += __fts5_get_poslist_size(&a[i], &n_pos, &dummy);
      i += n_pos;
    }
    if( i>=n ) break;
    i += __fts5_get_varint(&a[i], &i_delta);
    iter->i_rowid += i_delta;

    /* If necessary, grow the iter->rowid_offsets[] array. */
    if( i_rowid_offset>=iter->n_rowid_offset ){
      int64_t n_new = iter->n_rowid_offset + 8;
      int *news = (int*)dbsql_realloc64(iter->rowid_offsets,n_new*sizeof(int));
      if( news==0 ){
        p->rc = DBSQL_NOMEM;
        break;
      }
      iter->rowid_offsets = news;
      iter->n_rowid_offset = n_new;
    }

    iter->rowid_offsets[i_rowid_offset++] = iter->i_leaf_offset;
    iter->i_leaf_offset = i;
  }
  iter->i_rowid_offset = i_rowid_offset;
  __fts5_seg_iter_load_n_pos(p, iter);
}

/*
**
*/
static void __fts5_seg_iter_reverse_new_page(fts5_index_t *p, fts5_seg_iter_t *iter){
  assert( iter->flags & FTS5_SEGITER_REVERSE );
  assert( iter->flags & FTS5_SEGITER_ONETERM );

  __fts5_data_release(iter->leaf);
  iter->leaf = 0;
  while( p->rc==DBSQL_OK && iter->i_leaf_pgno>iter->i_term_leaf_pgno ){
    fts5_data_t *p_new;
    iter->i_leaf_pgno--;
    p_new = __fts5_leaf_read(p, FTS5_SEGMENT_ROWID(
          iter->seg->i_segid, iter->i_leaf_pgno
    ));
    if( p_new ){
      /* i_term_leaf_offset may be equal to sz_leaf if the term is the last
      ** thing on the page - i.e. the first rowid is on the following page.
      ** In this case leave iter->leaf==0, this iterator is at EOF. */
      if( iter->i_leaf_pgno==iter->i_term_leaf_pgno ){
        assert( iter->leaf==0 );
        if( iter->i_term_leaf_offset<p_new->sz_leaf ){
          iter->leaf = p_new;
          iter->i_leaf_offset = iter->i_term_leaf_offset;
        }
      }else{
        int i_rowid_off;
        i_rowid_off = FTS5_LEAF_FIRST_ROWID_OFF(p_new);
        if( i_rowid_off ){
          if( i_rowid_off>=p_new->sz_leaf ){
            FTS5_CORRUPT_ITER(p, iter);
          }else{
            iter->leaf = p_new;
            iter->i_leaf_offset = i_rowid_off;
          }
        }
      }

      if( iter->leaf ){
        u_int8_t *a = &iter->leaf->p[iter->i_leaf_offset];
        iter->i_leaf_offset += __fts5_get_varint(a, (u_int64_t*)&iter->i_rowid);
        break;
      }else{
        __fts5_data_release(p_new);
      }
    }
  }

  if( iter->leaf ){
    iter->i_endof_doclist = iter->leaf->nn+1;
    __fts5_seg_iter_reverse_init_page(p, iter);
  }
}

/*
** Return true if the iterator passed as the second argument currently
** points to a delete marker. A delete marker is an entry with a 0 byte
** position-list.
*/
static int __fts5_multi_iter_is_empty(fts5_index_t *p, fts5_iter_t *iter){
  fts5_seg_iter_t *seg = &iter->segs[iter->firsts[1].i_first];
  return (p->rc==DBSQL_OK && seg->leaf && seg->n_pos==0);
}

/*
** Advance iterator iter to the next entry.
**
** This version of __fts5_seg_iter_next() is only used by reverse iterators.
*/
static void __fts5_seg_iter_next_reverse(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter,             /* Iterator to advance */
  int *pb_unused                   /* Unused */
){
  assert( iter->flags & FTS5_SEGITER_REVERSE );
  assert( iter->next_leaf==0 );
  UNUSED_PARAM(pb_unused);

  if( iter->i_rowid_offset>0 ){
    u_int8_t *a = iter->leaf->p;
    int i_off;
    u_int64_t i_delta;

    iter->i_rowid_offset--;
    iter->i_leaf_offset = iter->rowid_offsets[iter->i_rowid_offset];
    __fts5_seg_iter_load_n_pos(p, iter);
    i_off = iter->i_leaf_offset;
    if( p->config->detail!=FTS5_DETAIL_NONE ){
      i_off += iter->n_pos;
    }
    __fts5_get_varint(&a[i_off], &i_delta);
    iter->i_rowid -= i_delta;
  }else{
    __fts5_seg_iter_reverse_new_page(p, iter);
  }
}

/*
** Advance iterator iter to the next entry.
**
** This version of __fts5_seg_iter_next() is only used if detail=none and the
** iterator is not a reverse direction iterator.
*/
static void __fts5_seg_iter_next_none(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter,             /* Iterator to advance */
  int *pb_new_term                  /* OUT: Set for new term */
){
  int i_off;

  assert( p->rc==DBSQL_OK );
  assert( (iter->flags & FTS5_SEGITER_REVERSE)==0 );
  assert( p->config->detail==FTS5_DETAIL_NONE );

  ASSERT_SZLEAF_OK(iter->leaf);
  i_off = iter->i_leaf_offset;

  /* Next entry is on the next page */
  while( iter->seg && i_off>=iter->leaf->sz_leaf ){
    __fts5_seg_iter_next_page(p, iter);
    if( p->rc || iter->leaf==0 ) return;
    iter->i_rowid = 0;
    i_off = 4;
  }

  if( i_off<iter->i_endof_doclist ){
    /* Next entry is on the current page */
    u_int64_t i_delta;
    i_off += __fts5_get_varint(&iter->leaf->p[i_off], (u_int64_t*)&i_delta);
    iter->i_leaf_offset = i_off;
    iter->i_rowid += i_delta;
  }else if( (iter->flags & FTS5_SEGITER_ONETERM)==0 ){
    if( iter->seg ){
      int n_keep = 0;
      if( i_off!=__fts5_leaf_first_term_off(iter->leaf) ){
        i_off += FTS5_GET_VARINT32(&iter->leaf->p[i_off], n_keep);
      }
      iter->i_leaf_offset = i_off;
      __fts5_seg_iter_load_term(p, iter, n_keep);
    }else{
      const u_int8_t *list = 0;
      const char *term = 0;
      int n_term = 0;
      int n_list;
      __fts5_hash_scan_next(p->hash);
      __fts5_hash_scan_entry(p->hash, &term, &n_term, &list, &n_list);
      if( list==0 ) goto next_none_eof;
      iter->leaf->p = (u_int8_t*)list;
      iter->leaf->nn = n_list;
      iter->leaf->sz_leaf = n_list;
      iter->i_endof_doclist = n_list;
      __fts5_buffer_set(&p->rc,&iter->term, n_term, (u_int8_t*)term);
      iter->i_leaf_offset = __fts5_get_varint(list, (u_int64_t*)&iter->i_rowid);
    }

    if( pb_new_term ) *pb_new_term = 1;
  }else{
    goto next_none_eof;
  }

  __fts5_seg_iter_load_n_pos(p, iter);

  return;
 next_none_eof:
  __fts5_data_release(iter->leaf);
  iter->leaf = 0;
}


/*
** Advance iterator iter to the next entry. 
**
** If an error occurs, fts5_index_t.rc is set to an appropriate error code. It 
** is not considered an error if the iterator reaches EOF. If an error has 
** already occurred when this function is called, it is a no-op.
*/
static void __fts5_seg_iter_next(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter,             /* Iterator to advance */
  int *pb_new_term                  /* OUT: Set for new term */
){
  fts5_data_t *leaf = iter->leaf;
  int i_off;
  int new_term = 0;
  int n_keep = 0;
  u_int8_t *a;
  int n;

  assert( pb_new_term==0 || *pb_new_term==0 );
  assert( p->config->detail!=FTS5_DETAIL_NONE );

  /* Search for the end of the position list within the current page. */
  a = leaf->p;
  n = leaf->sz_leaf;

  ASSERT_SZLEAF_OK(leaf);
  i_off = iter->i_leaf_offset + iter->n_pos;

  if( i_off<n ){
    /* The next entry is on the current page. */
    assert_nc( i_off<=iter->i_endof_doclist );
    if( i_off>=iter->i_endof_doclist ){
      new_term = 1;
      if( i_off!=__fts5_leaf_first_term_off(leaf) ){
        i_off += FTS5_GET_VARINT32(&a[i_off], n_keep);
      }
    }else{
      u_int64_t i_delta;
      i_off += __fts5_get_varint(&a[i_off], &i_delta);
      iter->i_rowid += i_delta;
      assert_nc( i_delta>0 );
    }
    iter->i_leaf_offset = i_off;

  }else if( iter->seg==0 ){
    const u_int8_t *list = 0;
    const char *term = 0;
    int n_term = 0;
    int n_list = 0;
    assert( (iter->flags & FTS5_SEGITER_ONETERM) || pb_new_term );
    if( 0==(iter->flags & FTS5_SEGITER_ONETERM) ){
      __fts5_hash_scan_next(p->hash);
      __fts5_hash_scan_entry(p->hash, &term, &n_term, &list, &n_list);
    }
    if( list==0 ){
      __fts5_data_release(iter->leaf);
      iter->leaf = 0;
    }else{
      iter->leaf->p = (u_int8_t*)list;
      iter->leaf->nn = n_list;
      iter->leaf->sz_leaf = n_list;
      iter->i_endof_doclist = n_list+1;
      __fts5_buffer_set(&p->rc, &iter->term, n_term, (u_int8_t*)term);
      iter->i_leaf_offset = __fts5_get_varint(list, (u_int64_t*)&iter->i_rowid);
      *pb_new_term = 1;
    }
  }else{
    i_off = 0;
    /* Next entry is not on the current page */
    while( i_off==0 ){
      __fts5_seg_iter_next_page(p, iter);
      leaf = iter->leaf;
      if( leaf==0 ) break;
      ASSERT_SZLEAF_OK(leaf);
      if( (i_off = FTS5_LEAF_FIRST_ROWID_OFF(leaf)) && i_off<leaf->sz_leaf ){
        i_off += __fts5_get_varint(&leaf->p[i_off], (u_int64_t*)&iter->i_rowid);
        iter->i_leaf_offset = i_off;

        if( leaf->nn>leaf->sz_leaf ){
          iter->i_pgidx_off = leaf->sz_leaf + FTS5_GET_VARINT32(
              &leaf->p[leaf->sz_leaf], iter->i_endof_doclist
          );
        }
      }
      else if( leaf->nn>leaf->sz_leaf ){
        iter->i_pgidx_off = leaf->sz_leaf + FTS5_GET_VARINT32(
            &leaf->p[leaf->sz_leaf], i_off
        );
        iter->i_leaf_offset = i_off;
        iter->i_endof_doclist = i_off;
        new_term = 1;
      }
      assert_nc( i_off<leaf->sz_leaf );
      if( i_off>leaf->sz_leaf ){
        FTS5_CORRUPT_ITER(p, iter);
        return;
      }
    }
  }

  /* Check if the iterator is now at EOF. If so, return early. */
  if( iter->leaf ){
    if( new_term ){
      if( iter->flags & FTS5_SEGITER_ONETERM ){
        __fts5_data_release(iter->leaf);
        iter->leaf = 0;
      }else{
        __fts5_seg_iter_load_term(p, iter, n_keep);
        __fts5_seg_iter_load_n_pos(p, iter);
        if( pb_new_term ) *pb_new_term = 1;
      }
    }else{
      /* The following could be done by calling __fts5_seg_iter_load_n_pos(). But
      ** this block is particularly performance critical, so equivalent
      ** code is inlined.  */
      int n_sz;
      assert_nc( iter->i_leaf_offset<=iter->leaf->nn );
      FTS5_FAST_GET_VARINT32(iter->leaf->p, iter->i_leaf_offset, n_sz);
      iter->del = (n_sz & 0x0001);
      iter->n_pos = n_sz>>1;
      assert_nc( iter->n_pos>=0 );
    }
  }
}

#define SWAPVAL(T, a, b) { T tmp; tmp=a; a=b; b=tmp; }

#define FTS5_INDEX_SKIP_VARINT(a, i_off) {            \
  int i_end = i_off+9;                              \
  while( (a[i_off++] & 0x80) && i_off<i_end );       \
}

/*
** Iterator iter currently points to the first rowid in a doclist. This
** function sets the iterator up so that iterates in reverse order through
** the doclist.
*/
static void __fts5_seg_iter_reverse(fts5_index_t *p, fts5_seg_iter_t *iter){
  fts5_dlidx_iter_t *dlidx = iter->dlidx;
  fts5_data_t *last = 0;
  int pgno_last = 0;

  if( dlidx && p->config->i_version==FTS5_CURRENT_VERSION ){
    int i_segid = iter->seg->i_segid;
    pgno_last = __fts5_dlidx_iter_pgno(dlidx);
    last = __fts5_leaf_read(p, FTS5_SEGMENT_ROWID(i_segid, pgno_last));
  }else{
    fts5_data_t *leaf = iter->leaf;         /* Current leaf data */

    /* Currently, fts5_seg_iter_t.i_leaf_offset points to the first byte of
    ** position-list content for the current rowid. Back it up so that it
    ** points to the start of the position-list size field. */
    int i_poslist;
    if( iter->i_term_leaf_pgno==iter->i_leaf_pgno ){
      i_poslist = iter->i_term_leaf_offset;
    }else{
      i_poslist = 4;
    }
    FTS5_INDEX_SKIP_VARINT(leaf->p, i_poslist);
    iter->i_leaf_offset = i_poslist;

    /* If this condition is true then the largest rowid for the current
    ** term may not be stored on the current page. So search forward to
    ** see where said rowid really is.  */
    if( iter->i_endof_doclist>=leaf->sz_leaf ){
      int pgno;
      fts5_structure_segment_t *seg = iter->seg;

      /* The last rowid in the doclist may not be on the current page. Search
      ** forward to find the page containing the last rowid.  */
      for(pgno=iter->i_leaf_pgno+1; !p->rc && pgno<=seg->pgno_last; pgno++){
        int64_t i_abs = FTS5_SEGMENT_ROWID(seg->i_segid, pgno);
        fts5_data_t *p_new = __fts5_leaf_read(p, i_abs);
        if( p_new ){
          int i_rowid, termless;
          i_rowid = FTS5_LEAF_FIRST_ROWID_OFF(p_new);
          termless = FTS5_LEAF_IS_TERMLESS(p_new);
          if( i_rowid ){
            SWAPVAL(fts5_data_t*, p_new, last);
            pgno_last = pgno;
          }
          __fts5_data_release(p_new);
          if( termless==0 ) break;
        }
      }
    }
  }

  /* If last is NULL at this point, then the last rowid for this doclist
  ** lies on the page currently indicated by the iterator. In this case 
  ** iter->i_leaf_offset is already set to point to the position-list size
  ** field associated with the first relevant rowid on the page.
  **
  ** Or, if last is non-NULL, then it is the page that contains the last
  ** rowid. In this case configure the iterator so that it points to the
  ** first rowid on this page.
  */
  if( last ){
    int i_off;
    __fts5_data_release(iter->leaf);
    iter->leaf = last;
    iter->i_leaf_pgno = pgno_last;
    if( p->rc==DBSQL_OK ){
      i_off = FTS5_LEAF_FIRST_ROWID_OFF(last);
      if( i_off>last->sz_leaf ){
        FTS5_CORRUPT_ITER(p, iter);
        return;
      }
      i_off += __fts5_get_varint(&last->p[i_off], (u_int64_t*)&iter->i_rowid);
      iter->i_leaf_offset = i_off;

      if( FTS5_LEAF_IS_TERMLESS(last) ){
        iter->i_endof_doclist = last->nn+1;
      }else{
        iter->i_endof_doclist = __fts5_leaf_first_term_off(last);
      }
    }
  }

  __fts5_seg_iter_reverse_init_page(p, iter);
}

/*
** Iterator iter currently points to the first rowid of a doclist.
** There is a doclist-index associated with the final term on the current 
** page. If the current term is the last term on the page, load the 
** doclist-index from disk and initialize an iterator at (iter->dlidx).
*/
static void __fts5_seg_iter_load_dlidx(fts5_index_t *p, fts5_seg_iter_t *iter){
  int i_seg = iter->seg->i_segid;
  int rev = (iter->flags & FTS5_SEGITER_REVERSE);
  fts5_data_t *leaf = iter->leaf; /* Current leaf data */

  assert( iter->flags & FTS5_SEGITER_ONETERM );
  assert( iter->dlidx==0 );

  /* Check if the current doclist ends on this page. If it does, return
  ** early without loading the doclist-index (as it belongs to a different
  ** term. */
  if( iter->i_term_leaf_pgno==iter->i_leaf_pgno 
   && iter->i_endof_doclist<leaf->sz_leaf 
  ){
    return;
  }

  iter->dlidx = __fts5_dlidx_iter_init(p, rev, i_seg, iter->i_term_leaf_pgno);
}

/*
** The iterator object passed as the second argument currently contains
** no valid values except for the fts5_seg_iter_t.leaf member variable. This
** function searches the leaf page for a term matching (term/n_term).
**
** If the specified term is found on the page, then the iterator is left
** pointing to it. If argument ge is zero and the term is not found,
** the iterator is left pointing at EOF.
**
** If ge is non-zero and the specified term is not found, then the
** iterator is left pointing to the smallest term in the segment that
** is larger than the specified term, even if this term is not on the
** current page.
*/
static void __fts5_leaf_seek(
  fts5_index_t *p,                   /* Leave any error code here */
  int ge,                        /* True for a >= search */
  fts5_seg_iter_t *iter,             /* Iterator to seek */
  const u_int8_t *term, int n_term      /* Term to search for */
){
  u_int32_t i_off;
  const u_int8_t *a = iter->leaf->p;
  u_int32_t n = (u_int32_t)iter->leaf->nn;

  u_int32_t n_match = 0;
  u_int32_t n_keep = 0;
  u_int32_t n_new = 0;
  u_int32_t i_term_off;
  u_int32_t i_pgidx;                     /* Current offset in pgidx */
  int end_of_page = 0;

  assert( p->rc==DBSQL_OK );

  i_pgidx = (u_int32_t)iter->leaf->sz_leaf;
  i_pgidx += FTS5_GET_VARINT32(&a[i_pgidx], i_term_off);
  i_off = i_term_off;
  if( i_off>n ){
    FTS5_CORRUPT_ITER(p, iter);
    return;
  }

  while( 1 ){

    /* Figure out how many new bytes are in this term */
    FTS5_FAST_GET_VARINT32(a, i_off, n_new);
    if( n_keep<n_match ){
      goto search_failed;
    }
    if( (i_off+n_new)>n ){
      FTS5_CORRUPT_ITER(p, iter);
      return;
    }

    assert( n_keep>=n_match );
    if( n_keep==n_match ){
      u_int32_t n_cmp;
      u_int32_t i;
      n_cmp = (u_int32_t)MIN(n_new, n_term-n_match);
      for(i=0; i<n_cmp; i++){
        if( a[i_off+i]!=term[n_match+i] ) break;
      }
      n_match += i;

      if( (u_int32_t)n_term==n_match ){
        if( i==n_new ){
          goto search_success;
        }else{
          goto search_failed;
        }
      }else if( i<n_new && a[i_off+i]>term[n_match] ){
        goto search_failed;
      }
    }

    if( i_pgidx>=n ){
      end_of_page = 1;
      break;
    }

    i_pgidx += FTS5_GET_VARINT32(&a[i_pgidx], n_keep);
    i_term_off += n_keep;
    i_off = i_term_off;

    if( i_off>=n ){
      FTS5_CORRUPT_ITER(p, iter);
      return;
    }

    /* Read the n_keep field of the next term. */
    FTS5_FAST_GET_VARINT32(a, i_off, n_keep);
  }

 search_failed:
  if( ge==0 ){
    __fts5_data_release(iter->leaf);
    iter->leaf = 0;
    return;
  }else if( end_of_page ){
    do {
      __fts5_seg_iter_next_page(p, iter);
      if( iter->leaf==0 ) return;
      a = iter->leaf->p;
      if( FTS5_LEAF_IS_TERMLESS(iter->leaf)==0 ){
        i_pgidx = (u_int32_t)iter->leaf->sz_leaf;
        i_pgidx += FTS5_GET_VARINT32(&iter->leaf->p[i_pgidx], i_off);
        if( i_off<4 || (int64_t)i_off>=iter->leaf->sz_leaf ){
          FTS5_CORRUPT_ITER(p, iter);
          return;
        }else{
          n_keep = 0;
          i_term_off = i_off;
          n = (u_int32_t)iter->leaf->nn;
          i_off += FTS5_GET_VARINT32(&a[i_off], n_new);
          break;
        }
      }
    }while( 1 );
  }

 search_success:
  if( (int64_t)i_off+n_new>n || n_new<1 ){
    FTS5_CORRUPT_ITER(p, iter);
    return;
  }
  iter->i_leaf_offset = i_off + n_new;
  iter->i_term_leaf_offset = iter->i_leaf_offset;
  iter->i_term_leaf_pgno = iter->i_leaf_pgno;

  FTS5_BUFFER_SET(&p->rc, &iter->term, n_keep, term);
  FTS5_BUFFER_APPEND_BLOB(&p->rc, &iter->term, n_new, &a[i_off]);

  if( i_pgidx>=n ){
    iter->i_endof_doclist = iter->leaf->nn+1;
  }else{
    int n_extra;
    i_pgidx += FTS5_GET_VARINT32(&a[i_pgidx], n_extra);
    iter->i_endof_doclist = i_term_off + n_extra;
  }
  iter->i_pgidx_off = i_pgidx;

  __fts5_seg_iter_load_rowid(p, iter);
  __fts5_seg_iter_load_n_pos(p, iter);
}

static dbsql_stmt_t *__fts5_idx_select_stmt(fts5_index_t *p){
  if( p->idx_select==0 ){
    fts5_config_t *config = p->config;
    __fts5_index_prepare_stmt(p, &p->idx_select, dbsql_mprintf(
          "SELECT pgno FROM '%q'.'%q_idx' WHERE "
          "segid=? AND term<=? ORDER BY term DESC LIMIT 1",
          config->z_db, config->name
    ));
  }
  return p->idx_select;
}

/*
** Initialize the object iter to point to term term/n_term within segment
** seg. If there is no such term in the index, the iterator is set to EOF.
**
** If an error occurs, fts5_index_t.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void __fts5_seg_iter_seek_init(
  fts5_index_t *p,                   /* FTS5 backend */
  const u_int8_t *term, int n_term,     /* Term to seek to */
  int flags,                      /* Mask of FTS5INDEX_XXX flags */
  fts5_structure_segment_t *seg,     /* Description of segment */
  fts5_seg_iter_t *iter              /* Object to populate */
){
  int i_pg = 1;
  int ge = (flags & FTS5INDEX_QUERY_SCAN);
  int dlidx = 0;                 /* True if there is a doclist-index */
  dbsql_stmt_t *idx_select = 0;

  assert( ge==0 || (flags & FTS5INDEX_QUERY_DESC)==0 );
  assert( term && n_term );
  memset(iter, 0, sizeof(*iter));
  iter->seg = seg;

  /* This block sets stack variable i_pg to the leaf page number that may
  ** contain term (term/n_term), if it is present in the segment. */
  idx_select = __fts5_idx_select_stmt(p);
  if( p->rc ) return;
  dbsql_bind_int(idx_select, 1, seg->i_segid);
  dbsql_bind_blob(idx_select, 2, term, n_term, DBSQL_STATIC);
  if( DBSQL_ROW==dbsql_step(idx_select) ){
    int64_t val = dbsql_column_int(idx_select, 0);
    i_pg = (int)(val>>1);
    dlidx = (val & 0x0001);
  }
  p->rc = dbsql_reset(idx_select);
  dbsql_bind_null(idx_select, 2);

  if( i_pg<seg->pgno_first ){
    i_pg = seg->pgno_first;
    dlidx = 0;
  }

  iter->i_leaf_pgno = i_pg - 1;
  __fts5_seg_iter_next_page(p, iter);

  if( iter->leaf ){
    __fts5_leaf_seek(p, ge, iter, term, n_term);
  }

  if( p->rc==DBSQL_OK && (ge==0 || (flags & FTS5INDEX_QUERY_SCANONETERM)) ){
    iter->flags |= FTS5_SEGITER_ONETERM;
    if( iter->leaf ){
      if( flags & FTS5INDEX_QUERY_DESC ){
        iter->flags |= FTS5_SEGITER_REVERSE;
      }
      if( dlidx ){
        __fts5_seg_iter_load_dlidx(p, iter);
      }
      if( flags & FTS5INDEX_QUERY_DESC ){
        __fts5_seg_iter_reverse(p, iter);
      }
    }
  }

  __fts5_seg_iter_set_next(p, iter);
  if( 0==(flags & FTS5INDEX_QUERY_SCANONETERM) ){
    __fts5_seg_iter_alloc_tombstone(p, iter);
  }

  /* Either:
  **
  **   1) an error has occurred, or
  **   2) the iterator points to EOF, or
  **   3) the iterator points to an entry with term (term/n_term), or
  **   4) the FTS5INDEX_QUERY_SCAN flag was set and the iterator points
  **      to an entry with a term greater than or equal to (term/n_term).
  */
  assert_nc( p->rc!=DBSQL_OK                                       /* 1 */
   || iter->leaf==0                                               /* 2 */
   || __fts5_buffer_compare_blob(&iter->term, term, n_term)==0          /* 3 */
   || (ge && __fts5_buffer_compare_blob(&iter->term, term, n_term)>0)  /* 4 */
  );
}


/*
** SQL used by __fts5_seg_iter_next_init() to find the page to open.
*/
static dbsql_stmt_t *__fts5_idx_next_stmt(fts5_index_t *p){
  if( p->idx_next_select==0 ){
    fts5_config_t *config = p->config;
    __fts5_index_prepare_stmt(p, &p->idx_next_select, dbsql_mprintf(
          "SELECT pgno FROM '%q'.'%q_idx' WHERE "
          "segid=? AND term>? ORDER BY term ASC LIMIT 1",
          config->z_db, config->name
    ));
    
  }
  return p->idx_next_select;
}

/*
** This is similar to __fts5_seg_iter_seek_init(), except that it initializes
** the segment iterator to point to the first term following the page
** with token/n_token on it.
*/
static void __fts5_seg_iter_next_init(
  fts5_index_t *p, 
  const char *term, int n_term,
  fts5_structure_segment_t *seg,     /* Description of segment */
  fts5_seg_iter_t *iter              /* Object to populate */
){
  int i_pg = -1;                   /* Page of segment to open */
  int dlidx = 0;
  dbsql_stmt_t *sel = 0;         /* SELECT to find i_pg */

  sel = __fts5_idx_next_stmt(p);
  if( sel ){
    assert( p->rc==DBSQL_OK );
    dbsql_bind_int(sel, 1, seg->i_segid);
    dbsql_bind_blob(sel, 2, term, n_term, DBSQL_STATIC);

    if( dbsql_step(sel)==DBSQL_ROW ){
      int64_t val = dbsql_column_int64(sel, 0);
      i_pg = (int)(val>>1);
      dlidx = (val & 0x0001);
    }
    p->rc = dbsql_reset(sel);
    dbsql_bind_null(sel, 2);
    if( p->rc ) return;
  }

  memset(iter, 0, sizeof(*iter));
  iter->seg = seg;
  iter->flags |= FTS5_SEGITER_ONETERM;
  if( i_pg>=0 ){
    iter->i_leaf_pgno = i_pg - 1;
    __fts5_seg_iter_next_page(p, iter);
    __fts5_seg_iter_set_next(p, iter);
  }
  if( iter->leaf ){
    const u_int8_t *a = iter->leaf->p;
    int i_term_off = 0;

    iter->i_pgidx_off = iter->leaf->sz_leaf;
    iter->i_pgidx_off += FTS5_GET_VARINT32(&a[iter->i_pgidx_off], i_term_off);
    if( i_term_off > iter->leaf->sz_leaf ){
      p->rc = FTS5_CORRUPT;
      return;
    }
    iter->i_leaf_offset = i_term_off;
    __fts5_seg_iter_load_term(p, iter, 0);
    __fts5_seg_iter_load_n_pos(p, iter);
    if( dlidx ) __fts5_seg_iter_load_dlidx(p, iter);

    assert( p->rc!=DBSQL_OK || 
        __fts5_buffer_compare_blob(&iter->term, (const u_int8_t*)term, n_term)>0
    );
  }
}

/*
** Initialize the object iter to point to term term/n_term within the
** in-memory hash table. If there is no such term in the hash-table, the 
** iterator is set to EOF.
**
** If an error occurs, fts5_index_t.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void __fts5_seg_iter_hash_init(
  fts5_index_t *p,                   /* FTS5 backend */
  const u_int8_t *term, int n_term,     /* Term to seek to */
  int flags,                      /* Mask of FTS5INDEX_XXX flags */
  fts5_seg_iter_t *iter              /* Object to populate */
){
  int n_list = 0;
  const u_int8_t *z = 0;
  int n = 0;
  fts5_data_t *leaf = 0;

  assert( p->hash );
  assert( p->rc==DBSQL_OK );

  if( term==0 || (flags & FTS5INDEX_QUERY_SCAN) ){
    const u_int8_t *list = 0;

    p->rc = __fts5_hash_scan_init(p->hash, (const char*)term, n_term);
    __fts5_hash_scan_entry(p->hash, (const char**)&z, &n, &list, &n_list);
    if( list ){
      leaf = __fts5_idx_malloc(p, sizeof(fts5_data_t));
      if( leaf ){
        leaf->p = (u_int8_t*)list;
      }
    }

    /* The call to __fts5_hash_scan_init() causes the hash table to
    ** fill the size field of all existing position lists. This means they
    ** can no longer be appended to. Since the only scenario in which they
    ** can be appended to is if the previous operation on this table was
    ** a DELETE, by clearing the fts5_index_t.b_delete flag we can avoid this
    ** possibility altogether.  */
    p->b_delete = 0;
  }else{
    p->rc = __fts5_hash_query(p->hash, sizeof(fts5_data_t), 
        (const char*)term, n_term, (void**)&leaf, &n_list
    );
    if( leaf ){
      leaf->p = (u_int8_t*)&leaf[1];
    }
    z = term;
    n = n_term;
    iter->flags |= FTS5_SEGITER_ONETERM;
  }

  if( leaf ){
    __fts5_buffer_set(&p->rc, &iter->term, n, z);
    leaf->nn = leaf->sz_leaf = n_list;
    iter->leaf = leaf;
    iter->i_leaf_offset = __fts5_get_varint(leaf->p, (u_int64_t*)&iter->i_rowid);
    iter->i_endof_doclist = leaf->nn;

    if( flags & FTS5INDEX_QUERY_DESC ){
      iter->flags |= FTS5_SEGITER_REVERSE;
      __fts5_seg_iter_reverse_init_page(p, iter);
    }else{
      __fts5_seg_iter_load_n_pos(p, iter);
    }
  }

  __fts5_seg_iter_set_next(p, iter);
}

/*
** Array ap[] contains n elements. Release each of these elements using
** __fts5_data_release(). Then free the array itself using dbsql_free().
*/
static void __fts5_index_free_array(fts5_data_t **ap, int n){
  if( ap ){
    int ii;
    for(ii=0; ii<n; ii++){
      __fts5_data_release(ap[ii]);
    }
    dbsql_free(ap);
  }
}

/*
** Decrement the ref-count of the object passed as the only argument. If it
** reaches 0, free it and its contents. 
*/
static void __fts5_tombstone_array_delete(fts5_tombstone_array_t *p){
  if( p ){
    p->n_ref--;
    if( p->n_ref<=0 ){
      int ii;
      for(ii=0; ii<p->n_tombstone; ii++){
        __fts5_data_release(p->tombstones[ii]);
      }
      dbsql_free(p);
    }
  }
}

/*
** Zero the iterator passed as the only argument.
*/
static void __fts5_seg_iter_clear(fts5_seg_iter_t *iter){
  FTS5_BUFFER_FREE(&iter->term);
  __fts5_data_release(iter->leaf);
  __fts5_data_release(iter->next_leaf);
  __fts5_tombstone_array_delete(iter->tomb_array);
  __fts5_dlidx_iter_free(iter->dlidx);
  dbsql_free(iter->rowid_offsets);
  memset(iter, 0, sizeof(fts5_seg_iter_t));
}

#ifdef DBSQL_DEBUG

/*
** This function is used as part of the big assert() procedure implemented by
** FTS5_ASSERT_MULTI_ITER_SETUP(). It ensures that the result currently stored
** in *res is the correct result of comparing the current positions of the
** two iterators.
*/
static void __fts5_assert_comparison_result(
  fts5_iter_t *iter, 
  fts5_seg_iter_t *p1,
  fts5_seg_iter_t *p2,
  fts5_c_result_t *p_res
){
  int i1 = p1 - iter->segs;
  int i2 = p2 - iter->segs;

  if( p1->leaf || p2->leaf ){
    if( p1->leaf==0 ){
      assert( p_res->i_first==i2 );
    }else if( p2->leaf==0 ){
      assert( p_res->i_first==i1 );
    }else{
      int n_min = MIN(p1->term.n, p2->term.n);
      int res = FTS5_MEMCMP(p1->term.p, p2->term.p, n_min);
      if( res==0 ) res = p1->term.n - p2->term.n;

      if( res==0 ){
        assert( p_res->term_eq==1 );
        assert( p1->i_rowid!=p2->i_rowid );
        res = ((p1->i_rowid > p2->i_rowid)==iter->rev) ? -1 : 1;
      }else{
        assert( p_res->term_eq==0 );
      }

      if( res<0 ){
        assert( p_res->i_first==i1 );
      }else{
        assert( p_res->i_first==i2 );
      }
    }
  }
}

/*
** This function is a no-op unless DBSQL_DEBUG is defined when this module
** is compiled. In that case, this function is essentially an assert() 
** statement used to verify that the contents of the iter->firsts[] array
** are correct.
*/
static void FTS5_ASSERT_MULTI_ITER_SETUP(fts5_index_t *p, fts5_iter_t *iter){
  if( p->rc==DBSQL_OK ){
    fts5_seg_iter_t *first = &iter->segs[ iter->firsts[1].i_first ];
    int i;

    assert( (first->leaf==0)==iter->base.eof );

    /* Check that iter->i_switch_rowid is set correctly. */
    for(i=0; i<iter->n_seg; i++){
      fts5_seg_iter_t *p1 = &iter->segs[i];
      assert( p1==first 
           || p1->leaf==0 
           || __fts5_buffer_compare(&first->term, &p1->term) 
           || p1->i_rowid==iter->i_switch_rowid
           || (p1->i_rowid<iter->i_switch_rowid)==iter->rev
      );
    }

    for(i=0; i<iter->n_seg; i+=2){
      fts5_seg_iter_t *p1 = &iter->segs[i];
      fts5_seg_iter_t *p2 = &iter->segs[i+1];
      fts5_c_result_t *res = &iter->firsts[(iter->n_seg + i) / 2];
      __fts5_assert_comparison_result(iter, p1, p2, res);
    }

    for(i=1; i<(iter->n_seg / 2); i+=2){
      fts5_seg_iter_t *p1 = &iter->segs[ iter->firsts[i*2].i_first ];
      fts5_seg_iter_t *p2 = &iter->segs[ iter->firsts[i*2+1].i_first ];
      fts5_c_result_t *res = &iter->firsts[i];
      __fts5_assert_comparison_result(iter, p1, p2, res);
    }
  }
}
#else
# define FTS5_ASSERT_MULTI_ITER_SETUP(x,y)
#endif

/*
** Do the comparison necessary to populate iter->firsts[i_out].
**
** If the returned value is non-zero, then it is the index of an entry
** in the iter->segs[] array that is (a) not at EOF, and (b) pointing
** to a key that is a duplicate of another, higher priority, 
** segment-iterator in the seg->segs[] array.
*/
static int __fts5_multi_iter_do_compare(fts5_iter_t *iter, int i_out){
  int i1;                         /* index_t of left-hand fts5_seg_iter_t */
  int i2;                         /* index_t of right-hand fts5_seg_iter_t */
  int i_res;
  fts5_seg_iter_t *p1;                /* Left-hand fts5_seg_iter_t */
  fts5_seg_iter_t *p2;                /* Right-hand fts5_seg_iter_t */
  fts5_c_result_t *p_res = &iter->firsts[i_out];

  assert( i_out<iter->n_seg && i_out>0 );
  assert( iter->rev==0 || iter->rev==1 );

  if( i_out>=(iter->n_seg/2) ){
    i1 = (i_out - iter->n_seg/2) * 2;
    i2 = i1 + 1;
  }else{
    i1 = iter->firsts[i_out*2].i_first;
    i2 = iter->firsts[i_out*2+1].i_first;
  }
  p1 = &iter->segs[i1];
  p2 = &iter->segs[i2];

  p_res->term_eq = 0;
  if( p1->leaf==0 ){           /* If p1 is at EOF */
    i_res = i2;
  }else if( p2->leaf==0 ){     /* If p2 is at EOF */
    i_res = i1;
  }else{
    int res = __fts5_buffer_compare(&p1->term, &p2->term);
    if( res==0 ){
      assert_nc( i2>i1 );
      assert_nc( i2!=0 );
      p_res->term_eq = 1;
      if( p1->i_rowid==p2->i_rowid ){
        return i2;
      }
      res = ((p1->i_rowid > p2->i_rowid)==iter->rev) ? -1 : +1;
    }
    assert( res!=0 );
    if( res<0 ){
      i_res = i1;
    }else{
      i_res = i2;
    }
  }

  p_res->i_first = (u_int16_t)i_res;
  return 0;
}

/*
** Move the seg-iter so that it points to the first rowid on page i_leaf_pgno.
** It is an error if leaf i_leaf_pgno does not exist. Unless the db is
** a 'secure-delete' db, if it contains no rowids then this is also an error.
*/
static void __fts5_seg_iter_goto_page(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter,             /* Iterator to advance */
  int i_leaf_pgno
){
  assert( i_leaf_pgno>iter->i_leaf_pgno );

  if( i_leaf_pgno>iter->seg->pgno_last ){
    FTS5_CORRUPT_IDX(p);
  }else{
    __fts5_data_release(iter->next_leaf);
    iter->next_leaf = 0;
    iter->i_leaf_pgno = i_leaf_pgno-1;

    while( p->rc==DBSQL_OK ){
      int i_off;
      __fts5_seg_iter_next_page(p, iter);
      if( iter->leaf==0 ) break;
      i_off = FTS5_LEAF_FIRST_ROWID_OFF(iter->leaf);
      if( i_off>0 ){
        u_int8_t *a = iter->leaf->p;
        int n = iter->leaf->sz_leaf;
        if( i_off<4 || i_off>=n ){
          FTS5_CORRUPT_IDX(p);
        }else{
          i_off += __fts5_get_varint(&a[i_off], (u_int64_t*)&iter->i_rowid);
          iter->i_leaf_offset = i_off;
          __fts5_seg_iter_load_n_pos(p, iter);
        }
        break;
      }
    }
  }
}

/*
** Advance the iterator passed as the second argument until it is at or 
** past rowid i_from. Regardless of the value of i_from, the iterator is
** always advanced at least once.
*/
static void __fts5_seg_iter_next_from(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_iter_t *iter,             /* Iterator to advance */
  int64_t i_match                      /* Advance iterator at least this far */
){
  int rev = (iter->flags & FTS5_SEGITER_REVERSE);
  fts5_dlidx_iter_t *dlidx = iter->dlidx;
  int i_leaf_pgno = iter->i_leaf_pgno;
  int move = 1;

  assert( iter->flags & FTS5_SEGITER_ONETERM );
  assert( iter->dlidx );
  assert( iter->leaf );

  if( rev==0 ){
    while( !__fts5_dlidx_iter_eof(p, dlidx) && i_match>__fts5_dlidx_iter_rowid(dlidx) ){
      i_leaf_pgno = __fts5_dlidx_iter_pgno(dlidx);
      __fts5_dlidx_iter_next(p, dlidx);
    }
    assert_nc( i_leaf_pgno>=iter->i_leaf_pgno || p->rc );
    if( i_leaf_pgno>iter->i_leaf_pgno ){
      __fts5_seg_iter_goto_page(p, iter, i_leaf_pgno);
      move = 0;
    }
  }else{
    assert( iter->next_leaf==0 );
    assert( i_match<iter->i_rowid );
    while( !__fts5_dlidx_iter_eof(p, dlidx) && i_match<__fts5_dlidx_iter_rowid(dlidx) ){
      __fts5_dlidx_iter_prev(p, dlidx);
    }
    i_leaf_pgno = __fts5_dlidx_iter_pgno(dlidx);

    assert( __fts5_dlidx_iter_eof(p, dlidx) || i_leaf_pgno<=iter->i_leaf_pgno );

    if( i_leaf_pgno<iter->i_leaf_pgno ){
      iter->i_leaf_pgno = i_leaf_pgno+1;
      __fts5_seg_iter_reverse_new_page(p, iter);
      move = 0;
    }
  }

  do{
    if( move && p->rc==DBSQL_OK ) iter->next(p, iter, 0);
    if( iter->leaf==0 ) break;
    if( rev==0 && iter->i_rowid>=i_match ) break;
    if( rev!=0 && iter->i_rowid<=i_match ) break;
    move = 1;
  }while( p->rc==DBSQL_OK );
}

/*
** Free the iterator object passed as the second argument.
*/
static void __fts5_multi_iter_free(fts5_iter_t *iter){
  if( iter ){
    int i;
    for(i=0; i<iter->n_seg; i++){
      __fts5_seg_iter_clear(&iter->segs[i]);
    }
    FTS5_BUFFER_FREE(&iter->poslist);
    dbsql_free(iter);
  }
}

static void __fts5_multi_iter_advanced(
  fts5_index_t *p,                   /* FTS5 backend to iterate within */
  fts5_iter_t *iter,                /* Iterator to update firsts[] array for */
  int i_changed,                   /* index_t of sub-iterator just advanced */
  int i_minset                     /* Minimum entry in firsts[] to set */
){
  int i;
  for(i=(iter->n_seg+i_changed)/2; i>=i_minset && p->rc==DBSQL_OK; i=i/2){
    int i_eq;
    if( (i_eq = __fts5_multi_iter_do_compare(iter, i)) ){
      fts5_seg_iter_t *seg = &iter->segs[i_eq];
      assert( p->rc==DBSQL_OK );
      seg->next(p, seg, 0);
      i = iter->n_seg + i_eq;
    }
  }
}

/*
** Sub-iterator i_changed of iterator iter has just been advanced. It still
** points to the same term though - just a different rowid. This function
** attempts to update the contents of the iter->firsts[] accordingly.
** If it does so successfully, 0 is returned. Otherwise 1.
**
** If non-zero is returned, the caller should call __fts5_multi_iter_advanced()
** on the iterator instead. That function does the same as this one, except
** that it deals with more complicated cases as well.
*/ 
static int __fts5_multi_iter_advance_rowid(
  fts5_iter_t *iter,                /* Iterator to update firsts[] array for */
  int i_changed,                   /* index_t of sub-iterator just advanced */
  fts5_seg_iter_t **first
){
  fts5_seg_iter_t *p_new = &iter->segs[i_changed];

  if( p_new->i_rowid==iter->i_switch_rowid
   || (p_new->i_rowid<iter->i_switch_rowid)==iter->rev
  ){
    int i;
    fts5_seg_iter_t *other = &iter->segs[i_changed ^ 0x0001];
    iter->i_switch_rowid = iter->rev ? SMALLEST_INT64 : LARGEST_INT64;
    for(i=(iter->n_seg+i_changed)/2; 1; i=i/2){
      fts5_c_result_t *res = &iter->firsts[i];

      assert( p_new->leaf );
      assert( res->term_eq==0 || other->leaf );

      if( res->term_eq ){
        if( p_new->i_rowid==other->i_rowid ){
          return 1;
        }else if( (other->i_rowid>p_new->i_rowid)==iter->rev ){
          iter->i_switch_rowid = other->i_rowid;
          p_new = other;
        }else if( (other->i_rowid>iter->i_switch_rowid)==iter->rev ){
          iter->i_switch_rowid = other->i_rowid;
        }
      }
      res->i_first = (u_int16_t)(p_new - iter->segs);
      if( i==1 ) break;

      other = &iter->segs[ iter->firsts[i ^ 0x0001].i_first ];
    }
  }

  *first = p_new;
  return 0;
}

/*
** Set the iter->eof variable based on the state of the sub-iterators.
*/
static void __fts5_multi_iter_set_eof(fts5_iter_t *iter){
  fts5_seg_iter_t *seg = &iter->segs[ iter->firsts[1].i_first ];
  iter->base.eof = seg->leaf==0;
  iter->i_switch_rowid = seg->i_rowid;
}

/*
** The argument to this macro must be an fts5_data_t structure containing a
** tombstone hash page. This macro returns the key-size of the hash-page.
*/
#define TOMBSTONE_KEYSIZE(pg) (pg->p[0]==4 ? 4 : 8)

#define TOMBSTONE_NSLOT(pg)   \
  ((pg->nn > 16) ? ((pg->nn-8) / TOMBSTONE_KEYSIZE(pg)) : 1)

/*
** Query a single tombstone hash table for rowid i_rowid. Return true if
** it is found or false otherwise. The tombstone hash table is one of
** n_hash_table tables.
*/
static int __fts5_index_tombstone_query(
  fts5_data_t *hash,                /* hash_t table page to query */
  int n_hash_table,                 /* Number of pages attached to segment */
  u_int64_t i_rowid                      /* Rowid to query hash for */
){
  const int sz_key = TOMBSTONE_KEYSIZE(hash);
  const int n_slot = TOMBSTONE_NSLOT(hash);
  int i_slot = (i_rowid / n_hash_table) % n_slot;
  int n_collide = n_slot;

  if( i_rowid==0 ){
    return hash->p[1];
  }else if( sz_key==4 ){
    u_int32_t *slots = (u_int32_t*)&hash->p[8];
    while( slots[i_slot] ){
      if( __fts5_get_u32((u_int8_t*)&slots[i_slot])==i_rowid ) return 1;
      if( n_collide--==0 ) break;
      i_slot = (i_slot+1)%n_slot;
    }
  }else{
    u_int64_t *slots = (u_int64_t*)&hash->p[8];
    while( slots[i_slot] ){
      if( __fts5_get_u64((u_int8_t*)&slots[i_slot])==i_rowid ) return 1;
      if( n_collide--==0 ) break;
      i_slot = (i_slot+1)%n_slot;
    }
  }

  return 0;
}

/*
** Return true if the iterator passed as the only argument points
** to an segment entry for which there is a tombstone. Return false
** if there is no tombstone or if the iterator is already at EOF.
*/
static int __fts5_multi_iter_is_deleted(fts5_iter_t *iter){
  int i_first = iter->firsts[1].i_first;
  fts5_seg_iter_t *seg = &iter->segs[i_first];
  fts5_tombstone_array_t *array = seg->tomb_array;

  if( seg->leaf && array ){
    /* Figure out which page the rowid might be present on. */
    int i_pg = ((u_int64_t)seg->i_rowid) % array->n_tombstone;
    assert( i_pg>=0 );

    /* If tombstone hash page i_pg has not yet been loaded from the 
    ** database, load it now. */
    if( array->tombstones[i_pg]==0 ){
      array->tombstones[i_pg] = __fts5_data_read(iter->p_index,
          FTS5_TOMBSTONE_ROWID(seg->seg->i_segid, i_pg)
      );
      if( array->tombstones[i_pg]==0 ) return 0;
    }

    return __fts5_index_tombstone_query(
        array->tombstones[i_pg],
        array->n_tombstone,
        seg->i_rowid
    );
  }

  return 0;
}

/*
** Move the iterator to the next entry. 
**
** If an error occurs, an error code is left in fts5_index_t.rc. It is not 
** considered an error if the iterator reaches EOF, or if it is already at 
** EOF when this function is called.
*/
static void __fts5_multi_iter_next(
  fts5_index_t *p, 
  fts5_iter_t *iter,
  int from,                      /* True if argument i_from is valid */
  int64_t i_from                       /* Advance at least as far as this */
){
  int use_from = from;
  assert( iter->base.eof==0 );
  while( p->rc==DBSQL_OK ){
    int i_first = iter->firsts[1].i_first;
    int new_term = 0;
    fts5_seg_iter_t *seg = &iter->segs[i_first];
    assert( p->rc==DBSQL_OK );
    if( use_from && seg->dlidx ){
      __fts5_seg_iter_next_from(p, seg, i_from);
    }else{
      seg->next(p, seg, &new_term);
    }

    if( seg->leaf==0 || new_term 
     || __fts5_multi_iter_advance_rowid(iter, i_first, &seg)
    ){
      __fts5_multi_iter_advanced(p, iter, i_first, 1);
      __fts5_multi_iter_set_eof(iter);
      seg = &iter->segs[iter->firsts[1].i_first];
      if( seg->leaf==0 ) return;
    }

    FTS5_ASSERT_MULTI_ITER_SETUP(p, iter);
    assert( seg==&iter->segs[iter->firsts[1].i_first] && seg->leaf );
    if( (iter->skip_empty==0 || seg->n_pos) 
      && 0==__fts5_multi_iter_is_deleted(iter)
    ){
      iter->set_outputs(iter, seg);
      return;
    }
    use_from = 0;
  }
}

static void __fts5_multi_iter_next2(
  fts5_index_t *p, 
  fts5_iter_t *iter,
  int *pb_new_term                  /* OUT: True if *might* be new term */
){
  assert( iter->skip_empty );
  if( p->rc==DBSQL_OK ){
    *pb_new_term = 0;
    do{
      int i_first = iter->firsts[1].i_first;
      fts5_seg_iter_t *seg = &iter->segs[i_first];
      int new_term = 0;

      assert( p->rc==DBSQL_OK );
      seg->next(p, seg, &new_term);
      if( seg->leaf==0 || new_term 
       || __fts5_multi_iter_advance_rowid(iter, i_first, &seg)
      ){
        __fts5_multi_iter_advanced(p, iter, i_first, 1);
        __fts5_multi_iter_set_eof(iter);
        *pb_new_term = 1;
      }
      FTS5_ASSERT_MULTI_ITER_SETUP(p, iter);

    }while( (__fts5_multi_iter_is_empty(p, iter) || __fts5_multi_iter_is_deleted(iter)) 
         && (p->rc==DBSQL_OK)
    );
  }
}

static void __fts5_iter_set_outputs_noop(fts5_iter_t *unused1, fts5_seg_iter_t *unused2){
  UNUSED_PARAM2(unused1, unused2);
}

static fts5_iter_t *__fts5_multi_iter_alloc(
  fts5_index_t *p,                   /* FTS5 backend to iterate within */
  int n_seg
){
  fts5_iter_t *p_new;
  int64_t n_slot;                      /* Power of two >= n_seg */

  for(n_slot=2; n_slot<n_seg; n_slot=n_slot*2);
  p_new = __fts5_idx_malloc(p, 
      SZ_FTS5ITER(n_slot) +                /* p_new + p_new->segs[] */
      sizeof(fts5_c_result_t) * n_slot         /* p_new->firsts[] */
  );
  if( p_new ){
    p_new->n_seg = n_slot;
    p_new->firsts = (fts5_c_result_t*)&p_new->segs[n_slot];
    p_new->p_index = p;
    p_new->set_outputs = __fts5_iter_set_outputs_noop;
  }
  return p_new;
}

static void __fts5_poslist_callback(
  fts5_index_t *unused, 
  void *context, 
  const u_int8_t *chunk, int n_chunk
){
  UNUSED_PARAM(unused);
  assert_nc( n_chunk>=0 );
  if( n_chunk>0 ){
    FTS5_BUFFER_SAFE_APPEND_BLOB((fts5_buffer_t*)context, chunk, n_chunk);
  }
}

typedef struct poslist_callback_ctx_t poslist_callback_ctx_t;
struct poslist_callback_ctx_t {
  fts5_buffer_t *buf;               /* Append to this buffer */
  fts5_colset_t *colset;            /* Restrict matches to this column */
  int state;                     /* See above */
};

typedef struct poslist_offsets_ctx_t poslist_offsets_ctx_t;
struct poslist_offsets_ctx_t {
  fts5_buffer_t *buf;               /* Append to this buffer */
  fts5_colset_t *colset;            /* Restrict matches to this column */
  int i_read;
  int i_write;
};

/*
** TODO: Make this more efficient!
*/
static int __fts5_index_colset_test(fts5_colset_t *colset, int i_col){
  int i;
  for(i=0; i<colset->n_col; i++){
    if( colset->cols[i]==i_col ) return 1;
  }
  return 0;
}

static void __fts5_poslist_offsets_callback(
  fts5_index_t *unused, 
  void *context, 
  const u_int8_t *chunk, int n_chunk
){
  poslist_offsets_ctx_t *ctx = (poslist_offsets_ctx_t*)context;
  UNUSED_PARAM(unused);
  assert_nc( n_chunk>=0 );
  if( n_chunk>0 ){
    int i = 0;
    while( i<n_chunk ){
      int i_val;
      i += FTS5_GET_VARINT32(&chunk[i], i_val);
      i_val += ctx->i_read - 2;
      ctx->i_read = i_val;
      if( __fts5_index_colset_test(ctx->colset, i_val) ){
        FTS5_BUFFER_SAFE_APPEND_VARINT(ctx->buf, i_val + 2 - ctx->i_write);
        ctx->i_write = i_val;
      }
    }
  }
}

static void __fts5_poslist_filter_callback(
  fts5_index_t *unused,
  void *context, 
  const u_int8_t *chunk, int n_chunk
){
  poslist_callback_ctx_t *ctx = (poslist_callback_ctx_t*)context;
  UNUSED_PARAM(unused);
  assert_nc( n_chunk>=0 );
  if( n_chunk>0 ){
    /* Search through to find the first varint with value 1. This is the
    ** start of the next columns hits. */
    int i = 0;
    int i_start = 0;

    if( ctx->state==2 ){
      int i_col;
      FTS5_FAST_GET_VARINT32(chunk, i, i_col);
      if( __fts5_index_colset_test(ctx->colset, i_col) ){
        ctx->state = 1;
        FTS5_BUFFER_SAFE_APPEND_VARINT(ctx->buf, 1);
      }else{
        ctx->state = 0;
      }
    }

    do {
      while( i<n_chunk && chunk[i]!=0x01 ){
        FTS5_INDEX_SKIP_VARINT(chunk, i);
      }
      if( ctx->state ){
        FTS5_BUFFER_SAFE_APPEND_BLOB(ctx->buf, &chunk[i_start], i-i_start);
      }
      if( i<n_chunk ){
        int i_col;
        i_start = i;
        i++;
        if( i>=n_chunk ){
          ctx->state = 2;
        }else{
          FTS5_FAST_GET_VARINT32(chunk, i, i_col);
          ctx->state = __fts5_index_colset_test(ctx->colset, i_col);
          if( ctx->state ){
            FTS5_BUFFER_SAFE_APPEND_BLOB(ctx->buf, &chunk[i_start], i-i_start);
            i_start = i;
          }
        }
      }
    }while( i<n_chunk );
  }
}

static void __fts5_chunk_iterate(
  fts5_index_t *p,                   /* index_t object */
  fts5_seg_iter_t *seg,              /* Poslist of this iterator */
  void *ctx,                     /* Context pointer for chunk callback */
  void (*x_chunk)(fts5_index_t*, void*, const u_int8_t*, int)
){
  int n_rem = seg->n_pos;          /* Number of bytes still to come */
  fts5_data_t *data = 0;
  u_int8_t *chunk = &seg->leaf->p[seg->i_leaf_offset];
  int n_chunk = MIN(n_rem, seg->leaf->sz_leaf - seg->i_leaf_offset);
  int pgno = seg->i_leaf_pgno;
  int pgno_save = 0;

  /* This function does not work with detail=none databases. */
  assert( p->config->detail!=FTS5_DETAIL_NONE );

  if( (seg->flags & FTS5_SEGITER_REVERSE)==0 ){
    pgno_save = pgno+1;
  }

  while( 1 ){
    x_chunk(p, ctx, chunk, n_chunk);
    n_rem -= n_chunk;
    __fts5_data_release(data);
    if( n_rem<=0 ){
      break;
    }else if( seg->seg==0 ){
      FTS5_CORRUPT_IDX(p);
      return;
    }else{
      pgno++;
      data = __fts5_leaf_read(p, FTS5_SEGMENT_ROWID(seg->seg->i_segid, pgno));
      if( data==0 ) break;
      chunk = &data->p[4];
      n_chunk = MIN(n_rem, data->sz_leaf - 4);
      if( pgno==pgno_save ){
        assert( seg->next_leaf==0 );
        seg->next_leaf = data;
        data = 0;
      }
    }
  }
}

/*
** Iterator iter currently points to a valid entry (not EOF). This
** function appends the position list data for the current entry to
** buffer buf. It does not make a copy of the position-list size
** field.
*/
static void __fts5_segiter_poslist(
  fts5_index_t *p,
  fts5_seg_iter_t *seg,
  fts5_colset_t *colset,
  fts5_buffer_t *buf
){
  assert( buf!=0 );
  assert( seg!=0 );
  if( 0==FTS5_BUFFER_GROW(&p->rc, buf, seg->n_pos+FTS5_DATA_ZERO_PADDING) ){
    assert( buf->p!=0 );
    assert( buf->n_space >= buf->n+seg->n_pos+FTS5_DATA_ZERO_PADDING );
    memset(&buf->p[buf->n+seg->n_pos], 0, FTS5_DATA_ZERO_PADDING);
    if( colset==0 ){
      __fts5_chunk_iterate(p, seg, (void*)buf, __fts5_poslist_callback);
    }else{
      if( p->config->detail==FTS5_DETAIL_FULL ){
        poslist_callback_ctx_t s_ctx;
        s_ctx.buf = buf;
        s_ctx.colset = colset;
        s_ctx.state = __fts5_index_colset_test(colset, 0);
        assert( s_ctx.state==0 || s_ctx.state==1 );
        __fts5_chunk_iterate(p, seg, (void*)&s_ctx, __fts5_poslist_filter_callback);
      }else{
        poslist_offsets_ctx_t s_ctx;
        memset(&s_ctx, 0, sizeof(s_ctx));
        s_ctx.buf = buf;
        s_ctx.colset = colset;
        __fts5_chunk_iterate(p, seg, (void*)&s_ctx, __fts5_poslist_offsets_callback);
      }
    }
  }
}

/*
** Parameter pos points to a buffer containing a position list, size n_pos.
** This function filters it according to colset (which must be non-NULL)
** and sets iter->base.data/n_data to point to the new position list.
** If memory is required for the new position list, use buffer iter->poslist.
** Or, if the new position list is a contiguous subset of the input, set
** iter->base.data/n_data to point directly to it.
**
** This function is a no-op if *rc is other than DBSQL_OK when it is
** called. If an OOM error is encountered, *rc is set to DBSQL_NOMEM
** before returning.
*/
static void __fts5_index_extract_colset(
  int *rc,
  fts5_colset_t *colset,            /* Colset to filter on */
  const u_int8_t *pos, int n_pos,       /* Position list */
  fts5_iter_t *iter
){
  if( *rc==DBSQL_OK ){
    const u_int8_t *p = pos;
    const u_int8_t *copies = p;
    const u_int8_t *end = &p[n_pos];    /* One byte past end of position list */
    int i = 0;
    int i_current = 0;

    if( colset->n_col>1 && __fts5_buffer_size(rc, &iter->poslist, n_pos) ){
      return;
    }

    while( 1 ){
      while( colset->cols[i]<i_current ){
        i++;
        if( i==colset->n_col ){
          iter->base.data = iter->poslist.p;
          iter->base.n_data = iter->poslist.n;
          return;
        }
      }

      /* Advance pointer p until it points to end or an 0x01 byte that is
      ** not part of a varint */
      while( p<end && *p!=0x01 ){
        while( p<end && (*p++ & 0x80) );
      }

      if( colset->cols[i]==i_current ){
        if( colset->n_col==1 ){
          iter->base.data = copies;
          iter->base.n_data = p-copies;
          return;
        }
        FTS5_BUFFER_SAFE_APPEND_BLOB(&iter->poslist, copies, p-copies);
      }
      if( p>=end ){
        iter->base.data = iter->poslist.p;
        iter->base.n_data = iter->poslist.n;
        return;
      }
      copies = p++;
      i_current = *p++;
      if( i_current & 0x80 ){
        p--;
        p += FTS5_GET_VARINT32(p, i_current);
      }
    }
  }

}

/*
** set_outputs callback used by detail=none tables.
*/
static void __fts5_iter_set_outputs_none(fts5_iter_t *iter, fts5_seg_iter_t *seg){
  assert( iter->p_index->config->detail==FTS5_DETAIL_NONE );
  iter->base.i_rowid = seg->i_rowid;
  iter->base.n_data = seg->n_pos;
}

/*
** set_outputs callback used by detail=full and detail=col tables when no
** column filters are specified.
*/
static void __fts5_iter_set_outputs_nocolset(fts5_iter_t *iter, fts5_seg_iter_t *seg){
  iter->base.i_rowid = seg->i_rowid;
  iter->base.n_data = seg->n_pos;

  assert( iter->p_index->config->detail!=FTS5_DETAIL_NONE );
  assert( iter->colset==0 );

  if( seg->i_leaf_offset+seg->n_pos<=seg->leaf->sz_leaf ){
    /* All data is stored on the current page. Populate the output 
    ** variables to point into the body of the page object. */
    iter->base.data = &seg->leaf->p[seg->i_leaf_offset];
  }else{
    /* The data is distributed over two or more pages. Copy it into the
    ** fts5_iter_t.poslist buffer and then set the output pointer to point
    ** to this buffer.  */
    FTS5_BUFFER_ZERO(&iter->poslist);
    __fts5_segiter_poslist(iter->p_index, seg, 0, &iter->poslist);
    iter->base.data = iter->poslist.p;
  }
}

/*
** set_outputs callback used when the fts5_colset_t object has n_col==0 (match
** against no columns at all).
*/
static void __fts5_iter_set_outputs_zero_colset(fts5_iter_t *iter, fts5_seg_iter_t *seg){
  UNUSED_PARAM(seg);
  iter->base.n_data = 0;
}

/*
** set_outputs callback used by detail=col when there is a column filter
** and there are 100 or more columns. Also called as a fallback from
** __fts5_iter_set_outputs_col100 if the column-list spans more than one page.
*/
static void __fts5_iter_set_outputs_col(fts5_iter_t *iter, fts5_seg_iter_t *seg){
  FTS5_BUFFER_ZERO(&iter->poslist);
  __fts5_segiter_poslist(iter->p_index, seg, iter->colset, &iter->poslist);
  iter->base.i_rowid = seg->i_rowid;
  iter->base.data = iter->poslist.p;
  iter->base.n_data = iter->poslist.n;
}

/*
** set_outputs callback used when: 
**
**   * detail=col,
**   * there is a column filter, and
**   * the table contains 100 or fewer columns. 
**
** The last point is to ensure all column numbers are stored as 
** single-byte varints.
*/
static void __fts5_iter_set_outputs_col100(fts5_iter_t *iter, fts5_seg_iter_t *seg){

  assert( iter->p_index->config->detail==FTS5_DETAIL_COLUMNS );
  assert( iter->colset );
  assert( iter->poslist.n_space>=iter->p_index->config->n_col );

  if( seg->i_leaf_offset+seg->n_pos>seg->leaf->sz_leaf 
   || seg->n_pos>iter->p_index->config->n_col
  ){
    __fts5_iter_set_outputs_col(iter, seg);
  }else{
    u_int8_t *a = (u_int8_t*)&seg->leaf->p[seg->i_leaf_offset];
    u_int8_t *end = (u_int8_t*)&a[seg->n_pos]; 
    int i_prev = 0;
    int *cols = iter->colset->cols;
    int *col_ends = &cols[iter->colset->n_col];

    u_int8_t *outs = iter->poslist.p;
    int i_prev_out = 0;

    iter->base.i_rowid = seg->i_rowid;

    while( a<end ){
      i_prev += (int)a++[0] - 2;
      while( *cols<i_prev ){
        cols++;
        if( cols==col_ends ) goto setoutputs_col_out;
      }
      if( *cols==i_prev ){
        *outs++ = (u_int8_t)((i_prev - i_prev_out) + 2);
        i_prev_out = i_prev;
      }
    }

setoutputs_col_out:
    iter->base.data = iter->poslist.p;
    iter->base.n_data = outs - iter->poslist.p;
  }
}

/*
** set_outputs callback used by detail=full when there is a column filter.
*/
static void __fts5_iter_set_outputs_full(fts5_iter_t *iter, fts5_seg_iter_t *seg){
  fts5_colset_t *colset = iter->colset;
  iter->base.i_rowid = seg->i_rowid;

  assert( iter->p_index->config->detail==FTS5_DETAIL_FULL );
  assert( colset );

  if( seg->i_leaf_offset+seg->n_pos<=seg->leaf->sz_leaf ){
    /* All data is stored on the current page. Populate the output 
    ** variables to point into the body of the page object. */
    const u_int8_t *a = &seg->leaf->p[seg->i_leaf_offset];
    int *rc = &iter->p_index->rc;
    FTS5_BUFFER_ZERO(&iter->poslist);
    __fts5_index_extract_colset(rc, colset, a, seg->n_pos, iter);
  }else{
    /* The data is distributed over two or more pages. Copy it into the
    ** fts5_iter_t.poslist buffer and then set the output pointer to point
    ** to this buffer.  */
    FTS5_BUFFER_ZERO(&iter->poslist);
    __fts5_segiter_poslist(iter->p_index, seg, colset, &iter->poslist);
    iter->base.data = iter->poslist.p;
    iter->base.n_data = iter->poslist.n;
  }
}

static void __fts5_iter_set_output_cb(int *rc, fts5_iter_t *iter){
  assert( iter!=0 || (*rc)!=DBSQL_OK );
  if( *rc==DBSQL_OK ){
    fts5_config_t *config = iter->p_index->config;
    if( config->detail==FTS5_DETAIL_NONE ){
      iter->set_outputs = __fts5_iter_set_outputs_none;
    }

    else if( iter->colset==0 ){
      iter->set_outputs = __fts5_iter_set_outputs_nocolset;
    }

    else if( iter->colset->n_col==0 ){
      iter->set_outputs = __fts5_iter_set_outputs_zero_colset;
    }

    else if( config->detail==FTS5_DETAIL_FULL ){
      iter->set_outputs = __fts5_iter_set_outputs_full;
    }

    else{
      assert( config->detail==FTS5_DETAIL_COLUMNS );
      if( config->n_col<=100 ){
        iter->set_outputs = __fts5_iter_set_outputs_col100;
        __fts5_buffer_size(rc, &iter->poslist, config->n_col);
      }else{
        iter->set_outputs = __fts5_iter_set_outputs_col;
      }
    }
  }
}

/*
** All the component segment-iterators of iter have been set up. This
** functions finishes setup for iterator iter itself.
*/
static void __fts5_multi_iter_finish_setup(fts5_index_t *p, fts5_iter_t *iter){
  int i_iter;
  for(i_iter=iter->n_seg-1; i_iter>0; i_iter--){
    int i_eq;
    if( (i_eq = __fts5_multi_iter_do_compare(iter, i_iter)) ){
      fts5_seg_iter_t *seg = &iter->segs[i_eq];
      if( p->rc==DBSQL_OK ) seg->next(p, seg, 0);
      __fts5_multi_iter_advanced(p, iter, i_eq, i_iter);
    }
  }
  __fts5_multi_iter_set_eof(iter);
  FTS5_ASSERT_MULTI_ITER_SETUP(p, iter);

  if( (iter->skip_empty && __fts5_multi_iter_is_empty(p, iter))
   || __fts5_multi_iter_is_deleted(iter)
  ){
    __fts5_multi_iter_next(p, iter, 0, 0);
  }else if( iter->base.eof==0 ){
    fts5_seg_iter_t *seg = &iter->segs[iter->firsts[1].i_first];
    iter->set_outputs(iter, seg);
  }
}

/*
** Allocate a new fts5_iter_t object.
**
** The new object will be used to iterate through data in structure p_struct.
** If i_level is -ve, then all data in all segments is merged. Or, if i_level
** is zero or greater, data from the first n_segment segments on level i_level
** is merged.
**
** The iterator initially points to the first term/rowid entry in the 
** iterated data.
*/
static void __fts5_multi_iter_new(
  fts5_index_t *p,                   /* FTS5 backend to iterate within */
  fts5_structure_t *p_struct,         /* Structure of specific index */
  int flags,                      /* FTS5INDEX_QUERY_XXX flags */
  fts5_colset_t *colset,            /* Colset to filter on (or NULL) */
  const u_int8_t *term, int n_term,     /* Term to seek to (or NULL/0) */
  int i_level,                     /* Level to iterate (-1 for all) */
  int n_segment,                   /* Number of segments to merge (i_level>=0) */
  fts5_iter_t **out                /* New object */
){
  int n_seg = 0;                   /* Number of segment-iters in use */
  int i_iter = 0;                  /* */
  int i_seg;                       /* Used to iterate through segments */
  fts5_structure_level_t *lvl;
  fts5_iter_t *p_new;

  assert( (term==0 && n_term==0) || i_level<0 );

  /* Allocate space for the new multi-seg-iterator. */
  if( p->rc==DBSQL_OK ){
    if( i_level<0 ){
      assert( p_struct->n_segment==__fts5_structure_count_segments(p_struct) );
      n_seg = p_struct->n_segment;
      n_seg += (p->hash && 0==(flags & FTS5INDEX_QUERY_SKIPHASH));
    }else{
      n_seg = MIN(p_struct->levels[i_level].n_seg, n_segment);
    }
  }
  *out = p_new = __fts5_multi_iter_alloc(p, n_seg);
  if( p_new==0 ){
    assert( p->rc!=DBSQL_OK );
    goto fts5_multi_iter_new_post_check;
  }
  p_new->rev = (0!=(flags & FTS5INDEX_QUERY_DESC));
  p_new->skip_empty = (0!=(flags & FTS5INDEX_QUERY_SKIPEMPTY));
  p_new->colset = colset;
  if( (flags & FTS5INDEX_QUERY_NOOUTPUT)==0 ){
    __fts5_iter_set_output_cb(&p->rc, p_new);
  }

  /* Initialize each of the component segment iterators. */
  if( p->rc==DBSQL_OK ){
    if( i_level<0 ){
      fts5_structure_level_t *end = &p_struct->levels[p_struct->n_level];
      if( p->hash && 0==(flags & FTS5INDEX_QUERY_SKIPHASH) ){
        /* Add a segment iterator for the current contents of the hash table. */
        fts5_seg_iter_t *iter = &p_new->segs[i_iter++];
        __fts5_seg_iter_hash_init(p, term, n_term, flags, iter);
      }
      for(lvl=&p_struct->levels[0]; lvl<end; lvl++){
        for(i_seg=lvl->n_seg-1; i_seg>=0; i_seg--){
          fts5_structure_segment_t *seg = &lvl->segs[i_seg];
          fts5_seg_iter_t *iter = &p_new->segs[i_iter++];
          if( term==0 ){
            __fts5_seg_iter_init(p, seg, iter);
          }else{
            __fts5_seg_iter_seek_init(p, term, n_term, flags, seg, iter);
          }
        }
      }
    }else{
      lvl = &p_struct->levels[i_level];
      for(i_seg=n_seg-1; i_seg>=0; i_seg--){
        __fts5_seg_iter_init(p, &lvl->segs[i_seg], &p_new->segs[i_iter++]);
      }
    }
    assert( i_iter==n_seg );
  }

  /* If the above was successful, each component iterator now points 
  ** to the first entry in its segment. In this case initialize the 
  ** firsts[] array. Or, if an error has occurred, free the iterator
  ** object and set the output variable to NULL.  */
  if( p->rc==DBSQL_OK ){
    __fts5_multi_iter_finish_setup(p, p_new);
  }else{
    __fts5_multi_iter_free(p_new);
    *out = 0;
  }

fts5_multi_iter_new_post_check:
  assert( (*out)!=0 || p->rc!=DBSQL_OK );
  return;
}

/*
** Create an fts5_iter_t that iterates through the doclist provided
** as the second argument.
*/
static void __fts5_multi_iter_new2(
  fts5_index_t *p,                   /* FTS5 backend to iterate within */
  fts5_data_t *data,                /* Doclist to iterate through */
  int desc,                      /* True for descending rowid order */
  fts5_iter_t **out                /* New object */
){
  fts5_iter_t *p_new;
  p_new = __fts5_multi_iter_alloc(p, 2);
  if( p_new ){
    fts5_seg_iter_t *iter = &p_new->segs[1];
    iter->flags = FTS5_SEGITER_ONETERM;
    if( data->sz_leaf>0 ){
      iter->leaf = data;
      iter->i_leaf_offset = __fts5_get_varint(data->p, (u_int64_t*)&iter->i_rowid);
      iter->i_endof_doclist = data->nn;
      p_new->firsts[1].i_first = 1;
      if( desc ){
        p_new->rev = 1;
        iter->flags |= FTS5_SEGITER_REVERSE;
        __fts5_seg_iter_reverse_init_page(p, iter);
      }else{
        __fts5_seg_iter_load_n_pos(p, iter);
      }
      data = 0;
    }else{
      p_new->base.eof = 1;
    }
    __fts5_seg_iter_set_next(p, iter);

    *out = p_new;
  }

  __fts5_data_release(data);
}

/*
** Return true if the iterator is at EOF or if an error has occurred. 
** False otherwise.
*/
static int __fts5_multi_iter_eof(fts5_index_t *p, fts5_iter_t *iter){
  assert( iter!=0 || p->rc!=DBSQL_OK );
  assert( p->rc!=DBSQL_OK
      || (iter->segs[ iter->firsts[1].i_first ].leaf==0)==iter->base.eof 
  );
  return (p->rc || iter->base.eof);
}

/*
** Return the rowid of the entry that the iterator currently points
** to. If the iterator points to EOF when this function is called the
** results are undefined.
*/
static int64_t __fts5_multi_iter_rowid(fts5_iter_t *iter){
  assert( iter->segs[ iter->firsts[1].i_first ].leaf );
  return iter->segs[ iter->firsts[1].i_first ].i_rowid;
}

/*
** Move the iterator to the next entry at or following i_match.
*/
static void __fts5_multi_iter_next_from(
  fts5_index_t *p, 
  fts5_iter_t *iter, 
  int64_t i_match
){
  while( 1 ){
    int64_t i_rowid;
    __fts5_multi_iter_next(p, iter, 1, i_match);
    if( __fts5_multi_iter_eof(p, iter) ) break;
    i_rowid = __fts5_multi_iter_rowid(iter);
    if( iter->rev==0 && i_rowid>=i_match ) break;
    if( iter->rev!=0 && i_rowid<=i_match ) break;
  }
}

/*
** Return a pointer to a buffer containing the term associated with the 
** entry that the iterator currently points to.
*/
static const u_int8_t *__fts5_multi_iter_term(fts5_iter_t *iter, int *pn){
  fts5_seg_iter_t *p = &iter->segs[ iter->firsts[1].i_first ];
  *pn = p->term.n;
  return p->term.p;
}

/*
** Allocate a new segment-id for the structure p_struct. The new segment
** id must be between 1 and 65335 inclusive, and must not be used by 
** any currently existing segment. If a free segment id cannot be found,
** DBSQL_FULL is returned.
**
** If an error has already occurred, this function is a no-op. 0 is 
** returned in this case.
*/
static int __fts5_allocate_segid(fts5_index_t *p, fts5_structure_t *p_struct){
  int i_segid = 0;

  if( p->rc==DBSQL_OK ){
    if( p_struct->n_segment>=FTS5_MAX_SEGMENT ){
      p->rc = DBSQL_FULL;
    }else{
      /* FTS5_MAX_SEGMENT is currently defined as 2000. So the following
      ** array is 63 elements, or 252 bytes, in size.  */
      u_int32_t useds[(FTS5_MAX_SEGMENT+31) / 32];
      int i_lvl, i_seg;
      int i;
      u_int32_t mask;
      memset(useds, 0, sizeof(useds));
      for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
        for(i_seg=0; i_seg<p_struct->levels[i_lvl].n_seg; i_seg++){
          int i_id = p_struct->levels[i_lvl].segs[i_seg].i_segid;
          if( i_id<=FTS5_MAX_SEGMENT && i_id>0 ){
            useds[(i_id-1) / 32] |= (u_int32_t)1 << ((i_id-1) % 32);
          }
        }
      }

      for(i=0; useds[i]==0xFFFFFFFF; i++);
      mask = useds[i];
      for(i_segid=0; mask & ((u_int32_t)1 << i_segid); i_segid++);
      i_segid += 1 + i*32;

#ifdef DBSQL_DEBUG
      for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
        for(i_seg=0; i_seg<p_struct->levels[i_lvl].n_seg; i_seg++){
          assert_nc( i_segid!=p_struct->levels[i_lvl].segs[i_seg].i_segid );
        }
      }
      assert_nc( i_segid>0 && i_segid<=FTS5_MAX_SEGMENT );

      {
        dbsql_stmt_t *idx_select = __fts5_idx_select_stmt(p);
        if( p->rc==DBSQL_OK ){
          u_int8_t blobs[2] = {0xff, 0xff};
          dbsql_bind_int(idx_select, 1, i_segid);
          dbsql_bind_blob(idx_select, 2, blobs, 2, DBSQL_STATIC);
          assert_nc( dbsql_step(idx_select)!=DBSQL_ROW );
          p->rc = dbsql_reset(idx_select);
          dbsql_bind_null(idx_select, 2);
        }
      }
#endif
    }
  }

  return i_segid;
}

/*
** Discard all data currently cached in the hash-tables.
*/
static void __fts5_index_discard_data(fts5_index_t *p){
  assert( p->hash || p->n_pending_data==0 );
  if( p->hash ){
    __fts5_hash_clear(p->hash);
    p->n_pending_data = 0;
    p->n_pending_row = 0;
    p->flush_rc = DBSQL_OK;
  }
  p->n_contentless_delete = 0;
}

/*
** Return the size of the prefix, in bytes, that buffer 
** (p_new/<length-unknown>) shares with buffer (old/n_old).
**
** Buffer (p_new/<length-unknown>) is guaranteed to be greater 
** than buffer (old/n_old).
*/
static int __fts5_prefix_compress(int n_old, const u_int8_t *old, const u_int8_t *p_new){
  int i;
  for(i=0; i<n_old; i++){
    if( old[i]!=p_new[i] ) break;
  }
  return i;
}

static void __fts5_write_dlidx_clear(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer,
  int flush                      /* If true, write dlidx to disk */
){
  int i;
  assert( flush==0 || (writer->n_dlidx>0 && writer->dlidxes[0].buf.n>0) );
  for(i=0; i<writer->n_dlidx; i++){
    fts5_dlidx_writer_t *dlidx = &writer->dlidxes[i];
    if( dlidx->buf.n==0 ) break;
    if( flush ){
      assert( dlidx->pgno!=0 );
      __fts5_data_write(p, 
          FTS5_DLIDX_ROWID(writer->i_segid, i, dlidx->pgno),
          dlidx->buf.p, dlidx->buf.n
      );
    }
    __fts5_buffer_zero(&dlidx->buf);
    dlidx->prev_valid = 0;
  }
}

/*
** Grow the writer->dlidxes[] array to at least n_lvl elements in size.
** Any new array elements are zeroed before returning.
*/
static int __fts5_write_dlidx_grow(
  fts5_index_t *p,
  fts5_seg_writer_t *writer,
  int n_lvl
){
  if( p->rc==DBSQL_OK && n_lvl>=writer->n_dlidx ){
    fts5_dlidx_writer_t *dlidxes = (fts5_dlidx_writer_t*)dbsql_realloc64(
        writer->dlidxes, sizeof(fts5_dlidx_writer_t) * n_lvl
    );
    if( dlidxes==0 ){
      p->rc = DBSQL_NOMEM;
    }else{
      size_t n_byte = sizeof(fts5_dlidx_writer_t) * (n_lvl - writer->n_dlidx);
      memset(&dlidxes[writer->n_dlidx], 0, n_byte);
      writer->dlidxes = dlidxes;
      writer->n_dlidx = n_lvl;
    }
  }
  return p->rc;
}

/*
** If the current doclist-index accumulating in writer->dlidxes[] is large
** enough, flush it to disk and return 1. Otherwise discard it and return
** zero.
*/
static int __fts5_write_flush_dlidx(fts5_index_t *p, fts5_seg_writer_t *writer){
  int flag = 0;

  /* If there were FTS5_MIN_DLIDX_SIZE or more empty leaf pages written
  ** to the database, also write the doclist-index to disk.  */
  if( writer->dlidxes[0].buf.n>0 && writer->n_empty>=FTS5_MIN_DLIDX_SIZE ){
    flag = 1;
  }
  __fts5_write_dlidx_clear(p, writer, flag);
  writer->n_empty = 0;
  return flag;
}

/*
** This function is called whenever processing of the doclist for the 
** last term on leaf page (writer->i_bt_page) is completed. 
**
** The doclist-index for that term is currently stored in-memory within the
** fts5_seg_writer_t.dlidxes[] array. If it is large enough, this function
** writes it out to disk. Or, if it is too small to bother with, discards
** it.
**
** fts5_seg_writer_t.btterm currently contains the first term on page i_bt_page.
*/
static void __fts5_write_flush_btree(fts5_index_t *p, fts5_seg_writer_t *writer){
  int flag;

  assert( writer->i_bt_page || writer->n_empty==0 );
  if( writer->i_bt_page==0 ) return;
  flag = __fts5_write_flush_dlidx(p, writer);

  if( p->rc==DBSQL_OK ){
    const char *z = (writer->btterm.n>0?(const char*)writer->btterm.p:"");
    /* The following was already done in __fts5_write_init(): */
    /* dbsql_bind_int(p->idx_writer, 1, writer->i_segid); */
    dbsql_bind_blob(p->idx_writer, 2, z, writer->btterm.n, DBSQL_STATIC);
    dbsql_bind_int64(p->idx_writer, 3, flag + ((int64_t)writer->i_bt_page<<1));
    dbsql_step(p->idx_writer);
    p->rc = dbsql_reset(p->idx_writer);
    dbsql_bind_null(p->idx_writer, 2);
  }
  writer->i_bt_page = 0;
}

/*
** This is called once for each leaf page except the first that contains
** at least one term. Argument (n_term/term) is the split-key - a term that
** is larger than all terms written to earlier leaves, and equal to or
** smaller than the first term on the new leaf.
**
** If an error occurs, an error code is left in fts5_index_t.rc. If an error
** has already occurred when this function is called, it is a no-op.
*/
static void __fts5_write_btree_term(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_writer_t *writer,         /* Writer object */
  int n_term, const u_int8_t *term      /* First term on new page */
){
  __fts5_write_flush_btree(p, writer);
  if( p->rc==DBSQL_OK ){
    FTS5_BUFFER_SET(&p->rc, &writer->btterm, n_term, term);
    writer->i_bt_page = writer->writer.pgno;
  }
}

/*
** This function is called when flushing a leaf page that contains no
** terms at all to disk.
*/
static void __fts5_write_btree_no_term(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_seg_writer_t *writer          /* Writer object */
){
  /* If there were no rowids on the leaf page either and the doclist-index
  ** has already been started, append an 0x00 byte to it.  */
  if( writer->first_rowid_in_page && writer->dlidxes[0].buf.n>0 ){
    fts5_dlidx_writer_t *dlidx = &writer->dlidxes[0];
    assert( dlidx->prev_valid );
    __fts5_buffer_append_varint(&p->rc, &dlidx->buf, 0);
  }

  /* Increment the "number of sequential leaves without a term" counter. */
  writer->n_empty++;
}

static int64_t __fts5_dlidx_extract_first_rowid(fts5_buffer_t *buf){
  int64_t i_rowid;
  int i_off;

  i_off = 1 + __fts5_get_varint(&buf->p[1], (u_int64_t*)&i_rowid);
  __fts5_get_varint(&buf->p[i_off], (u_int64_t*)&i_rowid);
  return i_rowid;
}

/*
** Rowid i_rowid has just been appended to the current leaf page. It is the
** first on the page. This function appends an appropriate entry to the current
** doclist-index.
*/
static void __fts5_write_dlidx_append(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer, 
  int64_t i_rowid
){
  int i;
  int done = 0;

  for(i=0; p->rc==DBSQL_OK && done==0; i++){
    int64_t i_val;
    fts5_dlidx_writer_t *dlidx = &writer->dlidxes[i];

    if( dlidx->buf.n>=p->config->pgsz ){
      /* The current doclist-index page is full. Write it to disk and push
      ** a copy of i_rowid (which will become the first rowid on the next
      ** doclist-index leaf page) up into the next level of the b-tree 
      ** hierarchy. If the node being flushed is currently the root node,
      ** also push its first rowid upwards. */
      dlidx->buf.p[0] = 0x01;    /* Not the root node */
      __fts5_data_write(p, 
          FTS5_DLIDX_ROWID(writer->i_segid, i, dlidx->pgno),
          dlidx->buf.p, dlidx->buf.n
      );
      __fts5_write_dlidx_grow(p, writer, i+2);
      dlidx = &writer->dlidxes[i];
      if( p->rc==DBSQL_OK && dlidx[1].buf.n==0 ){
        int64_t i_first = __fts5_dlidx_extract_first_rowid(&dlidx->buf);

        /* This was the root node. Push its first rowid up to the new root. */
        dlidx[1].pgno = dlidx->pgno;
        __fts5_buffer_append_varint(&p->rc, &dlidx[1].buf, 0);
        __fts5_buffer_append_varint(&p->rc, &dlidx[1].buf, dlidx->pgno);
        __fts5_buffer_append_varint(&p->rc, &dlidx[1].buf, i_first);
        dlidx[1].prev_valid = 1;
        dlidx[1].i_prev = i_first;
      }

      __fts5_buffer_zero(&dlidx->buf);
      dlidx->prev_valid = 0;
      dlidx->pgno++;
    }else{
      done = 1;
    }

    if( dlidx->prev_valid ){
      i_val = (u_int64_t)i_rowid - (u_int64_t)dlidx->i_prev;
    }else{
      int64_t i_pgno = (i==0 ? writer->writer.pgno : dlidx[-1].pgno);
      assert( dlidx->buf.n==0 );
      __fts5_buffer_append_varint(&p->rc, &dlidx->buf, !done);
      __fts5_buffer_append_varint(&p->rc, &dlidx->buf, i_pgno);
      i_val = i_rowid;
    }

    __fts5_buffer_append_varint(&p->rc, &dlidx->buf, i_val);
    dlidx->prev_valid = 1;
    dlidx->i_prev = i_rowid;
  }
}

static void __fts5_write_flush_leaf(fts5_index_t *p, fts5_seg_writer_t *writer){
  static const u_int8_t zero[] = { 0x00, 0x00, 0x00, 0x00 };
  fts5_page_writer_t *page = &writer->writer;
  int64_t i_rowid;

  assert( (page->pgidx.n==0)==(writer->first_term_in_page) );

  /* Set the sz_leaf header field. */
  assert( 0==__fts5_get_u16(&page->buf.p[2]) );
  __fts5_put_u16(&page->buf.p[2], (u_int16_t)page->buf.n);

  if( writer->first_term_in_page ){
    /* No term was written to this page. */
    assert( page->pgidx.n==0 );
    __fts5_write_btree_no_term(p, writer);
  }else{
    /* Append the pgidx to the page buffer. Set the sz_leaf header field. */
    FTS5_BUFFER_APPEND_BLOB(&p->rc, &page->buf, page->pgidx.n, page->pgidx.p);
  }

  /* Write the page out to disk */
  i_rowid = FTS5_SEGMENT_ROWID(writer->i_segid, page->pgno);
  __fts5_data_write(p, i_rowid, page->buf.p, page->buf.n);

  /* Initialize the next page. */
  FTS5_BUFFER_ZERO(&page->buf);
  FTS5_BUFFER_ZERO(&page->pgidx);
  FTS5_BUFFER_APPEND_BLOB(&p->rc, &page->buf, 4, zero);
  page->i_prev_pgidx = 0;
  page->pgno++;

  /* Increase the leaves written counter */
  writer->n_leaf_written++;

  /* The new leaf holds no terms or rowids */
  writer->first_term_in_page = 1;
  writer->first_rowid_in_page = 1;
}

/*
** Append term term/n_term to the segment being written by the writer passed
** as the second argument.
**
** If an error occurs, set the fts5_index_t.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void __fts5_write_append_term(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer,
  int n_term, const u_int8_t *term 
){
  int n_prefix;                    /* Bytes of prefix compression for term */
  fts5_page_writer_t *page = &writer->writer;
  fts5_buffer_t *pgidx = &writer->writer.pgidx;
  int n_min = MIN(page->term.n, n_term);

  assert( p->rc==DBSQL_OK );
  assert( page->buf.n>=4 );
  assert( page->buf.n>4 || writer->first_term_in_page );

  /* If the current leaf page is full, flush it to disk. */
  if( (page->buf.n + pgidx->n + n_term + 2)>=p->config->pgsz ){
    if( page->buf.n>4 ){
      __fts5_write_flush_leaf(p, writer);
      if( p->rc!=DBSQL_OK ) return;
    }
    FTS5_BUFFER_GROW(&p->rc, &page->buf, n_term+FTS5_DATA_PADDING);
  }
  
  /* TODO1: Updating pgidx here. */
  pgidx->n += __fts5_put_varint(
      &pgidx->p[pgidx->n], page->buf.n - page->i_prev_pgidx
  );
  page->i_prev_pgidx = page->buf.n;
#if 0
  __fts5_put_u16(&pgidx->p[pgidx->n], page->buf.n);
  pgidx->n += 2;
#endif

  if( writer->first_term_in_page ){
    n_prefix = 0;
    if( page->pgno!=1 ){
      /* This is the first term on a leaf that is not the leftmost leaf in
      ** the segment b-tree. In this case it is necessary to add a term to
      ** the b-tree hierarchy that is (a) larger than the largest term 
      ** already written to the segment and (b) smaller than or equal to
      ** this term. In other words, a prefix of (term/n_term) that is one
      ** byte longer than the longest prefix (term/n_term) shares with the
      ** previous term. 
      **
      ** Usually, the previous term is available in page->term. The exception
      ** is if this is the first term written in an incremental-merge step.
      ** In this case the previous term is not available, so just write a
      ** copy of (term/n_term) into the parent node. This is slightly
      ** inefficient, but still correct.  */
      int n = n_term;
      if( page->term.n ){
        n = 1 + __fts5_prefix_compress(n_min, page->term.p, term);
      }
      __fts5_write_btree_term(p, writer, n, term);
      if( p->rc!=DBSQL_OK ) return;
      page = &writer->writer;
    }
  }else{
    n_prefix = __fts5_prefix_compress(n_min, page->term.p, term);
    FTS5_BUFFER_APPEND_VARINT(&p->rc, &page->buf, n_prefix);
  }

  /* Append the number of bytes of new data, then the term data itself
  ** to the page. */
  FTS5_BUFFER_APPEND_VARINT(&p->rc, &page->buf, n_term - n_prefix);
  FTS5_BUFFER_APPEND_BLOB(&p->rc, &page->buf, n_term - n_prefix, &term[n_prefix]);

  /* Update the fts5_page_writer_t.term field. */
  FTS5_BUFFER_SET(&p->rc, &page->term, n_term, term);
  writer->first_term_in_page = 0;

  writer->first_rowid_in_page = 0;
  writer->first_rowid_in_doclist = 1;

  assert( p->rc || (writer->n_dlidx>0 && writer->dlidxes[0].buf.n==0) );
  writer->dlidxes[0].pgno = page->pgno;
}

/*
** Append a rowid and position-list size field to the writers output. 
*/
static void __fts5_write_append_rowid(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer,
  int64_t i_rowid
){
  if( p->rc==DBSQL_OK ){
    fts5_page_writer_t *page = &writer->writer;

    if( (page->buf.n + page->pgidx.n)>=p->config->pgsz ){
      __fts5_write_flush_leaf(p, writer);
    }

    /* If this is to be the first rowid written to the page, set the 
    ** rowid-pointer in the page-header. Also append a value to the dlidx
    ** buffer, in case a doclist-index is required.  */
    if( writer->first_rowid_in_page ){
      __fts5_put_u16(page->buf.p, (u_int16_t)page->buf.n);
      __fts5_write_dlidx_append(p, writer, i_rowid);
    }

    /* Write the rowid. */
    if( writer->first_rowid_in_doclist || writer->first_rowid_in_page ){
      FTS5_BUFFER_APPEND_VARINT(&p->rc, &page->buf, i_rowid);
    }else{
      assert_nc( p->rc || i_rowid>writer->i_prev_rowid );
      FTS5_BUFFER_APPEND_VARINT(&p->rc, &page->buf, 
          (u_int64_t)i_rowid - (u_int64_t)writer->i_prev_rowid
      );
    }
    writer->i_prev_rowid = i_rowid;
    writer->first_rowid_in_doclist = 0;
    writer->first_rowid_in_page = 0;
  }
}

static void __fts5_write_append_poslist_data(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer, 
  const u_int8_t *data, 
  int n_data
){
  fts5_page_writer_t *page = &writer->writer;
  const u_int8_t *a = data;
  int n = n_data;
  
  assert( p->config->pgsz>0 || p->rc!=DBSQL_OK );
  while( p->rc==DBSQL_OK 
     && (page->buf.n + page->pgidx.n + n)>=p->config->pgsz 
  ){
    int n_req = p->config->pgsz - page->buf.n - page->pgidx.n;
    int n_copy = 0;
    while( n_copy<n_req ){
      int64_t dummy;
      n_copy += __fts5_get_varint(&a[n_copy], (u_int64_t*)&dummy);
    }
    FTS5_BUFFER_APPEND_BLOB(&p->rc, &page->buf, n_copy, a);
    a += n_copy;
    n -= n_copy;
    __fts5_write_flush_leaf(p, writer);
  }
  if( n>0 ){
    FTS5_BUFFER_APPEND_BLOB(&p->rc, &page->buf, n, a);
  }
}

/*
** Flush any data cached by the writer object to the database. Free any
** allocations associated with the writer.
*/
static void __fts5_write_finish(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer,         /* Writer object */
  int *pn_leaf                     /* OUT: Number of leaf pages in b-tree */
){
  int i;
  fts5_page_writer_t *leaf = &writer->writer;
  if( p->rc==DBSQL_OK ){
    assert( leaf->pgno>=1 );
    if( leaf->buf.n>4 ){
      __fts5_write_flush_leaf(p, writer);
    }
    *pn_leaf = leaf->pgno-1;
    if( leaf->pgno>1 ){
      __fts5_write_flush_btree(p, writer);
    }
  }
  FTS5_BUFFER_FREE(&leaf->term);
  FTS5_BUFFER_FREE(&leaf->buf);
  FTS5_BUFFER_FREE(&leaf->pgidx);
  FTS5_BUFFER_FREE(&writer->btterm);

  for(i=0; i<writer->n_dlidx; i++){
    __fts5_buffer_free(&writer->dlidxes[i].buf);
  }
  dbsql_free(writer->dlidxes);
}

static void __fts5_write_init(
  fts5_index_t *p, 
  fts5_seg_writer_t *writer, 
  int i_segid
){
  const int n_buffer = p->config->pgsz + FTS5_DATA_PADDING;

  memset(writer, 0, sizeof(fts5_seg_writer_t));
  writer->i_segid = i_segid;

  __fts5_write_dlidx_grow(p, writer, 1);
  writer->writer.pgno = 1;
  writer->first_term_in_page = 1;
  writer->i_bt_page = 1;

  assert( writer->writer.buf.n==0 );
  assert( writer->writer.pgidx.n==0 );

  /* Grow the two buffers to pgsz + padding bytes in size. */
  __fts5_buffer_size(&p->rc, &writer->writer.pgidx, n_buffer);
  __fts5_buffer_size(&p->rc, &writer->writer.buf, n_buffer);

  if( p->idx_writer==0 ){
    fts5_config_t *config = p->config;
    __fts5_index_prepare_stmt(p, &p->idx_writer, dbsql_mprintf(
          "INSERT INTO '%q'.'%q_idx'(segid,term,pgno) VALUES(?,?,?)", 
          config->z_db, config->name
    ));
  }

  if( p->rc==DBSQL_OK ){
    /* Initialize the 4-byte leaf-page header to 0x00. */
    memset(writer->writer.buf.p, 0, 4);
    writer->writer.buf.n = 4;

    /* Bind the current output segment id to the index-writer. This is an
    ** optimization over binding the same value over and over as rows are
    ** inserted into %_idx by the current writer.  */
    dbsql_bind_int(p->idx_writer, 1, writer->i_segid);
  }
}

/*
** Iterator iter was used to iterate through the input segments of on an
** incremental merge operation. This function is called if the incremental
** merge step has finished but the input has not been completely exhausted.
*/
static void __fts5_trim_segments(fts5_index_t *p, fts5_iter_t *iter){
  int i;
  fts5_buffer_t buf;
  memset(&buf, 0, sizeof(fts5_buffer_t));
  for(i=0; i<iter->n_seg && p->rc==DBSQL_OK; i++){
    fts5_seg_iter_t *seg = &iter->segs[i];
    if( seg->seg==0 ){
      /* no-op */
    }else if( seg->leaf==0 ){
      /* All keys from this input segment have been transfered to the output.
      ** Set both the first and last page-numbers to 0 to indicate that the
      ** segment is now empty. */
      seg->seg->pgno_last = 0;
      seg->seg->pgno_first = 0;
    }else{
      int i_off = seg->i_term_leaf_offset;     /* Offset on new first leaf page */
      int64_t i_leaf_rowid;
      fts5_data_t *data;
      int i_id = seg->seg->i_segid;
      u_int8_t hdrs[4] = {0x00, 0x00, 0x00, 0x00};

      i_leaf_rowid = FTS5_SEGMENT_ROWID(i_id, seg->i_term_leaf_pgno);
      data = __fts5_leaf_read(p, i_leaf_rowid);
      if( data ){
        if( i_off>data->sz_leaf ){
          /* This can occur if the pages that the segments occupy overlap - if
          ** a single page has been assigned to more than one segment. In
          ** this case a prior iteration of this loop may have corrupted the
          ** segment currently being trimmed.  */
          FTS5_CORRUPT_ROWID(p, i_leaf_rowid);
        }else{
          FTS5_BUFFER_ZERO(&buf);
          FTS5_BUFFER_GROW(&p->rc, &buf, data->nn);
          FTS5_BUFFER_APPEND_BLOB(&p->rc, &buf, sizeof(hdrs), hdrs);
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, seg->term.n);
          FTS5_BUFFER_APPEND_BLOB(&p->rc, &buf, seg->term.n, seg->term.p);
          FTS5_BUFFER_APPEND_BLOB(&p->rc, &buf,data->sz_leaf-i_off,&data->p[i_off]);
          if( p->rc==DBSQL_OK ){
            /* Set the sz_leaf field */
            __fts5_put_u16(&buf.p[2], (u_int16_t)buf.n);
          }

          /* Set up the new page-index array */
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, 4);
          if( seg->i_leaf_pgno==seg->i_term_leaf_pgno 
           && seg->i_endof_doclist<data->sz_leaf
           && seg->i_pgidx_off<=data->nn
          ){
            int n_diff = data->sz_leaf - seg->i_endof_doclist;
            FTS5_BUFFER_APPEND_VARINT(&p->rc, &buf, buf.n - 1 - n_diff - 4);
            FTS5_BUFFER_APPEND_BLOB(&p->rc, &buf, 
                data->nn - seg->i_pgidx_off, &data->p[seg->i_pgidx_off]
            );
          }

          seg->seg->pgno_first = seg->i_term_leaf_pgno;
          __fts5_data_delete(p, FTS5_SEGMENT_ROWID(i_id, 1), i_leaf_rowid);
          __fts5_data_write(p, i_leaf_rowid, buf.p, buf.n);
        }
        __fts5_data_release(data);
      }
    }
  }
  FTS5_BUFFER_FREE(&buf);
}

static void __fts5_merge_chunk_callback(
  fts5_index_t *p, 
  void *ctx, 
  const u_int8_t *chunk, int n_chunk
){
  fts5_seg_writer_t *writer = (fts5_seg_writer_t*)ctx;
  __fts5_write_append_poslist_data(p, writer, chunk, n_chunk);
}

/*
**
*/
static void __fts5_index_merge_level(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_structure_t **pp_struct,       /* IN/OUT: Stucture of index */
  int i_lvl,                       /* Level to read input from */
  int *pn_rem                      /* Write up to this many output leaves */
){
  fts5_structure_t *p_struct = *pp_struct;
  fts5_structure_level_t *lvl = &p_struct->levels[i_lvl];
  fts5_structure_level_t *lvl_out;
  fts5_iter_t *iter = 0;       /* Iterator to read input data */
  int n_rem = pn_rem ? *pn_rem : 0;  /* Output leaf pages left to write */
  int n_input;                     /* Number of input segments */
  fts5_seg_writer_t writer;           /* Writer object */
  fts5_structure_segment_t *seg;     /* Output segment */
  fts5_buffer_t term;
  int oldest;                    /* True if the output segment is the oldest */
  int detail = p->config->detail;
  const int flags = FTS5INDEX_QUERY_NOOUTPUT;
  int term_written = 0;           /* True if current term already output */

  assert( i_lvl<p_struct->n_level );
  assert( lvl->n_merge<=lvl->n_seg );

  memset(&writer, 0, sizeof(fts5_seg_writer_t));
  memset(&term, 0, sizeof(fts5_buffer_t));
  if( lvl->n_merge ){
    lvl_out = &p_struct->levels[i_lvl+1];
    assert( lvl_out->n_seg>0 );
    n_input = lvl->n_merge;
    seg = &lvl_out->segs[lvl_out->n_seg-1];

    __fts5_write_init(p, &writer, seg->i_segid);
    writer.writer.pgno = seg->pgno_last+1;
    writer.i_bt_page = 0;
  }else{
    int i_segid = __fts5_allocate_segid(p, p_struct);

    /* Extend the fts5_structure_t object as required to ensure the output
    ** segment exists. */
    if( i_lvl==p_struct->n_level-1 ){
      __fts5_structure_add_level(&p->rc, pp_struct);
      p_struct = *pp_struct;
    }
    __fts5_structure_extend_level(&p->rc, p_struct, i_lvl+1, 1, 0);
    if( p->rc ) return;
    lvl = &p_struct->levels[i_lvl];
    lvl_out = &p_struct->levels[i_lvl+1];

    __fts5_write_init(p, &writer, i_segid);

    /* Add the new segment to the output level */
    seg = &lvl_out->segs[lvl_out->n_seg];
    lvl_out->n_seg++;
    seg->pgno_first = 1;
    seg->i_segid = i_segid;
    p_struct->n_segment++;

    /* Read input from all segments in the input level */
    n_input = lvl->n_seg;

    /* Set the range of origins that will go into the output segment. */
    if( p_struct->n_origin_cntr>0 ){
      seg->i_origin1 = lvl->segs[0].i_origin1;
      seg->i_origin2 = lvl->segs[lvl->n_seg-1].i_origin2;
    }
  }
  oldest = (lvl_out->n_seg==1 && p_struct->n_level==i_lvl+2);

  assert( i_lvl>=0 );
  for(__fts5_multi_iter_new(p, p_struct, flags, 0, 0, 0, i_lvl, n_input, &iter);
      __fts5_multi_iter_eof(p, iter)==0;
      __fts5_multi_iter_next(p, iter, 0, 0)
  ){
    fts5_seg_iter_t *seg_iter = &iter->segs[ iter->firsts[1].i_first ];
    int n_pos;                     /* position-list size field value */
    int n_term;
    const u_int8_t *p_term;

    p_term = __fts5_multi_iter_term(iter, &n_term);
    if( n_term!=term.n || FTS5_MEMCMP(p_term, term.p, n_term) ){
      if( pn_rem && writer.n_leaf_written>n_rem ){
        break;
      }
      FTS5_BUFFER_SET(&p->rc, &term, n_term, p_term);
      term_written =0;
    }

    /* Check for key annihilation. */
    if( seg_iter->n_pos==0 && (oldest || seg_iter->del==0) ) continue;

    if( p->rc==DBSQL_OK && term_written==0 ){
      /* This is a new term. Append a term to the output segment. */
      __fts5_write_append_term(p, &writer, n_term, p_term);
      term_written = 1;
    }

    /* Append the rowid to the output */
    /* WRITEPOSLISTSIZE */
    __fts5_write_append_rowid(p, &writer, __fts5_multi_iter_rowid(iter));

    if( detail==FTS5_DETAIL_NONE ){
      if( seg_iter->del ){
        FTS5_BUFFER_APPEND_VARINT(&p->rc, &writer.writer.buf, 0);
        if( seg_iter->n_pos>0 ){
          FTS5_BUFFER_APPEND_VARINT(&p->rc, &writer.writer.buf, 0);
        }
      }
    }else{
      /* Append the position-list data to the output */
      n_pos = seg_iter->n_pos*2 + seg_iter->del;
      FTS5_BUFFER_APPEND_VARINT(&p->rc, &writer.writer.buf, n_pos);
      __fts5_chunk_iterate(p, seg_iter, (void*)&writer, __fts5_merge_chunk_callback);
    }
  }

  /* Flush the last leaf page to disk. Set the output segment b-tree height
  ** and last leaf page number at the same time.  */
  __fts5_write_finish(p, &writer, &seg->pgno_last);

  assert( iter!=0 || p->rc!=DBSQL_OK );
  if( __fts5_multi_iter_eof(p, iter) ){
    int i;

    /* Remove the redundant segments from the %_data table */
    assert( seg->n_entry==0 );
    for(i=0; i<n_input; i++){
      fts5_structure_segment_t *old = &lvl->segs[i];
      seg->n_entry += (old->n_entry - old->n_entry_tombstone);
      __fts5_data_remove_segment(p, old);
    }

    /* Remove the redundant segments from the input level */
    if( lvl->n_seg!=n_input ){
      int n_move = (lvl->n_seg - n_input) * sizeof(fts5_structure_segment_t);
      memmove(lvl->segs, &lvl->segs[n_input], n_move);
    }
    p_struct->n_segment -= n_input;
    lvl->n_seg -= n_input;
    lvl->n_merge = 0;
    if( seg->pgno_last==0 ){
      lvl_out->n_seg--;
      p_struct->n_segment--;
    }
  }else{
    assert( seg->pgno_last>0 );
    __fts5_trim_segments(p, iter);
    lvl->n_merge = n_input;
  }

  __fts5_multi_iter_free(iter);
  FTS5_BUFFER_FREE(&term);
  if( pn_rem ) *pn_rem -= writer.n_leaf_written;
}

/*
** If this is not a contentless_delete=1 table, or if the 'deletemerge'
** configuration option is set to 0, then this function always returns -1.
** Otherwise, it searches the structure object passed as the second argument
** for a level suitable for merging due to having a large number of 
** tombstones in the tombstone hash. If one is found, its index is returned.
** Otherwise, if there is no suitable level, -1.
*/
static int __fts5_index_find_delete_merge(fts5_index_t *p, fts5_structure_t *p_struct){
  fts5_config_t *config = p->config;
  int i_ret = -1;
  if( config->contentless_delete && config->n_delete_merge>0 ){
    int ii;
    int n_best = 0;

    for(ii=0; ii<p_struct->n_level; ii++){
      fts5_structure_level_t *lvl = &p_struct->levels[ii];
      int64_t n_entry = 0;
      int64_t n_tomb = 0;
      int i_seg;
      for(i_seg=0; i_seg<lvl->n_seg; i_seg++){
        n_entry += lvl->segs[i_seg].n_entry;
        n_tomb += lvl->segs[i_seg].n_entry_tombstone;
      }
      assert_nc( n_entry>0 || lvl->n_seg==0 );
      if( n_entry>0 ){
        int n_percent = (n_tomb * 100) / n_entry;
        if( n_percent>=config->n_delete_merge && n_percent>n_best ){
          i_ret = ii;
          n_best = n_percent;
        }
      }

      /* If lvl is already the input level to an ongoing merge, look no
      ** further for a merge candidate. The caller should be allowed to
      ** continue merging from lvl first.  */
      if( lvl->n_merge ) break;
    }
  }
  return i_ret;
}

/*
** Do up to n_pg pages of automerge work on the index.
**
** Return true if any changes were actually made, or false otherwise.
*/
static int __fts5_index_merge_local(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_structure_t **pp_struct,       /* IN/OUT: Current structure of index */
  int n_pg,                        /* Pages of work to do */
  int n_min                        /* Minimum number of segments to merge */
){
  int n_rem = n_pg;
  int ret = 0;
  fts5_structure_t *p_struct = *pp_struct;
  while( n_rem>0 && p->rc==DBSQL_OK ){
    int i_lvl;                   /* To iterate through levels */
    int i_best_lvl = 0;           /* Level offering the most input segments */
    int n_best = 0;              /* Number of input segments on best level */

    /* Set i_best_lvl to the level to read input segments from. Or to -1 if
    ** there is no level suitable to merge segments from.  */
    assert( p_struct->n_level>0 );
    for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
      fts5_structure_level_t *lvl = &p_struct->levels[i_lvl];
      if( lvl->n_merge ){
        if( lvl->n_merge>n_best ){
          i_best_lvl = i_lvl;
          n_best = n_min;
        }
        break;
      }
      if( lvl->n_seg>n_best ){
        n_best = lvl->n_seg;
        i_best_lvl = i_lvl;
      }
    }
    if( n_best<n_min ){
      i_best_lvl = __fts5_index_find_delete_merge(p, p_struct);
    }

    if( i_best_lvl<0 ) break;
    ret = 1;
    __fts5_index_merge_level(p, &p_struct, i_best_lvl, &n_rem);
    if( p->rc==DBSQL_OK && p_struct->levels[i_best_lvl].n_merge==0 ){
      __fts5_structure_promote(p, i_best_lvl+1, p_struct);
    }

    if( n_min==1 ) n_min = 2;
  }
  *pp_struct = p_struct;
  return ret;
}

/*
** A total of n_leaf leaf pages of data has just been flushed to a level-0
** segment. This function updates the write-counter accordingly and, if
** necessary, performs incremental merge work.
**
** If an error occurs, set the fts5_index_t.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void __fts5_index_automerge(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_structure_t **pp_struct,       /* IN/OUT: Current structure of index */
  int n_leaf                       /* Number of output leaves just written */
){
  if( p->rc==DBSQL_OK && p->config->n_automerge>0 && ALWAYS((*pp_struct)!=0) ){
    fts5_structure_t *p_struct = *pp_struct;
    u_int64_t n_write;                   /* Initial value of write-counter */
    int n_work;                    /* Number of work-quanta to perform */
    int n_rem;                     /* Number of leaf pages left to write */

    /* Update the write-counter. While doing so, set n_work. */
    n_write = p_struct->n_write_counter;
    n_work = (int)(((n_write + n_leaf) / p->n_work_unit) - (n_write / p->n_work_unit));
    p_struct->n_write_counter += n_leaf;
    n_rem = (int)(p->n_work_unit * n_work * p_struct->n_level);

    __fts5_index_merge_local(p, pp_struct, n_rem, p->config->n_automerge);
  }
}

static void __fts5_index_crisismerge(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_structure_t **pp_struct        /* IN/OUT: Current structure of index */
){
  const int n_crisis = p->config->n_crisis_merge;
  fts5_structure_t *p_struct = *pp_struct;
  if( p_struct && p_struct->n_level>0 ){
    int i_lvl = 0;
    while( p->rc==DBSQL_OK && p_struct->levels[i_lvl].n_seg>=n_crisis ){
      __fts5_index_merge_level(p, &p_struct, i_lvl, 0);
      assert( p->rc!=DBSQL_OK || p_struct->n_level>(i_lvl+1) );
      __fts5_structure_promote(p, i_lvl+1, p_struct);
      i_lvl++;
    }
    *pp_struct = p_struct;
  }
}

static int __fts5_index_return(fts5_index_t *p){
  int rc = p->rc;
  p->rc = DBSQL_OK;
  return rc;
}

/*
** Close the read-only blob handle, if it is open.
*/
void __fts5_index_close_reader(fts5_index_t *p){
  __fts5_index_close_reader_local(p);
  __fts5_index_return(p);
}

typedef struct fts5_flush_ctx_t fts5_flush_ctx_t;
struct fts5_flush_ctx_t {
  fts5_index_t *idx;
  fts5_seg_writer_t writer; 
};

/*
** Buffer bufs[] contains a list of varints, all small enough to fit
** in a 32-bit integer. Return the size of the largest prefix of this 
** list n_max bytes or less in size.
*/
static int __fts5_poslist_prefix(const u_int8_t *bufs, int n_max){
  int ret;
  u_int32_t dummy;
  ret = FTS5_GET_VARINT32(bufs, dummy);
  if( ret<n_max ){
    while( 1 ){
      int i = FTS5_GET_VARINT32(&bufs[ret], dummy);
      if( (ret + i) > n_max ) break;
      ret += i;
    }
  }
  return ret;
}

/*
** Execute the SQL statement:
**
**    DELETE FROM %_idx WHERE (segid, (pgno/2)) = ($i_segid, $i_pgno);
**
** This is used when a secure-delete operation removes the last term
** from a segment leaf page. In that case the %_idx entry is removed 
** too. This is done to ensure that if all instances of a token are
** removed from an fts5 database in secure-delete mode, no trace of
** the token itself remains in the database.
*/
static void __fts5_secure_delete_idx_entry(
  fts5_index_t *p,                   /* FTS5 backend object */
  int i_segid,                     /* Id of segment to delete entry for */
  int i_pgno                       /* Page number within segment */
){
  if( i_pgno!=1 ){
    assert( p->config->i_version==FTS5_CURRENT_VERSION_SECUREDELETE );
    if( p->delete_from_idx==0 ){
      __fts5_index_prepare_stmt(p, &p->delete_from_idx, dbsql_mprintf(
          "DELETE FROM '%q'.'%q_idx' WHERE (segid, (pgno/2)) = (?1, ?2)",
          p->config->z_db, p->config->name
      ));
    }
    if( p->rc==DBSQL_OK ){
      dbsql_bind_int(p->delete_from_idx, 1, i_segid);
      dbsql_bind_int(p->delete_from_idx, 2, i_pgno);
      dbsql_step(p->delete_from_idx);
      p->rc = dbsql_reset(p->delete_from_idx);
    }
  }
}

/*
** This is called when a secure-delete operation removes a position-list
** that overflows onto segment page i_pgno of segment seg. This function
** rewrites node i_pgno, and possibly one or more of its right-hand peers,
** to remove this portion of the position list.
**
** Output variable (*pb_last_in_doclist) is set to true if the position-list
** removed is followed by a new term or the end-of-segment, or false if
** it is followed by another rowid/position list.
*/
static void __fts5_secure_delete_overflow(
  fts5_index_t *p,
  fts5_structure_segment_t *seg,
  int i_pgno,
  int *pb_last_in_doclist
){
  const int detail_none = (p->config->detail==FTS5_DETAIL_NONE);
  int pgno;
  fts5_data_t *leaf = 0;
  assert( i_pgno!=1 );

  *pb_last_in_doclist = 1;
  for(pgno=i_pgno; p->rc==DBSQL_OK && pgno<=seg->pgno_last; pgno++){
    int64_t i_rowid = FTS5_SEGMENT_ROWID(seg->i_segid, pgno);
    int i_next = 0;
    u_int8_t *pgs = 0;

    leaf = __fts5_leaf_read(p, i_rowid);
    if( leaf==0 ) break;
    pgs = leaf->p;

    i_next = __fts5_get_u16(&pgs[0]);
    if( i_next!=0 ){
      *pb_last_in_doclist = 0;
    }
    if( i_next==0 && leaf->sz_leaf<leaf->nn ){
      FTS5_GET_VARINT32(&pgs[leaf->sz_leaf], i_next);
    }

    if( i_next==0 ){
      /* The page contains no terms or rowids. Replace it with an empty
      ** page and move on to the right-hand peer.  */
      const u_int8_t empties[] = {0x00, 0x00, 0x00, 0x04}; 
      assert_nc( detail_none==0 || leaf->nn==4 );
      if( detail_none==0 ) __fts5_data_write(p, i_rowid, empties, sizeof(empties));
      __fts5_data_release(leaf);
      leaf = 0;
    }else if( detail_none ){
      break;
    }else if( i_next>=leaf->sz_leaf || leaf->nn<leaf->sz_leaf || i_next<4 ){
      FTS5_CORRUPT_ROWID(p, i_rowid);
      break;
    }else{
      int n_shift = i_next - 4;
      int n_pg;

      int n_idx = 0;
      u_int8_t *idx = 0;

      /* Unless the current page footer is 0 bytes in size (in which case
      ** the new page footer will be as well), allocate and populate a 
      ** buffer containing the new page footer. Set stack variables idx 
      ** and n_idx accordingly.  */
      if( leaf->nn>leaf->sz_leaf ){
        int i_first = 0;
        int i1 = leaf->sz_leaf;
        int i2 = 0;

        i1 += FTS5_GET_VARINT32(&pgs[i1], i_first);
        if( i_first<i_next ){
          FTS5_CORRUPT_ROWID(p, i_rowid);
          break;
        }
        idx = __fts5_malloc_zero(&p->rc, (leaf->nn-leaf->sz_leaf)+2);
        if( idx==0 ) break;
        i2 = __fts5_put_varint(idx, i_first-n_shift);
        if( i1<leaf->nn ){
          memcpy(&idx[i2], &pgs[i1], leaf->nn-i1);
          i2 += (leaf->nn-i1);
        }
        n_idx = i2;
      }

      /* Modify the contents of buffer pgs[]. Set n_pg to the new size 
      ** in bytes. The new page is always smaller than the old.  */
      n_pg = leaf->sz_leaf - n_shift;
      memmove(&pgs[4], &pgs[4+n_shift], n_pg-4);
      __fts5_put_u16(&pgs[2], n_pg);
      if( __fts5_get_u16(&pgs[0]) ) __fts5_put_u16(&pgs[0], 4);
      if( n_idx>0 ){
        memcpy(&pgs[n_pg], idx, n_idx);
        n_pg += n_idx;
      }
      dbsql_free(idx);

      /* Write the new page to disk and exit the loop */
      assert( n_pg>4 || __fts5_get_u16(pgs)==0 );
      __fts5_data_write(p, i_rowid, pgs, n_pg);
      break;
    }
  }
  __fts5_data_release(leaf);
}

/*
** Completely remove the entry that seg currently points to from 
** the database.
*/
static void __fts5_do_secure_delete(
  fts5_index_t *p,
  fts5_seg_iter_t *seg
){
  const int detail_none = (p->config->detail==FTS5_DETAIL_NONE);
  int i_segid = seg->seg->i_segid;
  u_int8_t *pgs = seg->leaf->p;
  int n_pg = seg->leaf->nn;
  int i_pg_idx = seg->leaf->sz_leaf;         /* Offset of page footer */

  u_int64_t i_delta = 0;
  int i_next_off = 0;
  int i_off = 0;
  int n_idx = 0;
  u_int8_t *idx = 0;
  int last_in_doclist = 0;
  int i_idx = 0;
  int i_start = 0;
  int i_del_key_off = 0;       /* Offset of deleted key, if any */

  n_idx = n_pg-i_pg_idx;
  idx = __fts5_malloc_zero(&p->rc, ((int64_t)n_idx)+16);
  if( p->rc ) return;
  memcpy(idx, &pgs[i_pg_idx], n_idx);

  /* At this point segment iterator seg points to the entry
  ** this function should remove from the b-tree segment. 
  **
  ** In detail=full or detail=column mode, seg->i_leaf_offset is the 
  ** offset of the first byte in the position-list for the entry to 
  ** remove. Immediately before this comes two varints that will also
  ** need to be removed:
  **
  **     + the rowid or delta rowid value for the entry, and
  **     + the size of the position list in bytes.
  **
  ** Or, in detail=none mode, there is a single varint prior to 
  ** seg->i_leaf_offset - the rowid or delta rowid value.
  **
  ** This block sets the following variables:
  **
  **   i_start:
  **     The offset of the first byte of the rowid or delta-rowid
  **     value for the doclist entry being removed.
  **
  **   i_delta:
  **     The value of the rowid or delta-rowid value for the doclist
  **     entry being removed.
  **
  **   i_next_off:
  **     The offset of the next entry following the position list
  **     for the one being removed. If the position list for this
  **     entry overflows onto the next leaf page, this value will be
  **     greater than leaf->sz_leaf.
  */
  {
    int i_sop;                     /* Start-Of-Position-list */
    if( seg->i_leaf_pgno==seg->i_term_leaf_pgno ){
      i_start = seg->i_term_leaf_offset;
    }else{
      i_start = __fts5_get_u16(&pgs[0]);
    }
    if( i_start>n_pg ){
      FTS5_CORRUPT_IDX(p);
      dbsql_free(idx);
      return;
    }

    i_sop = i_start + __fts5_get_varint(&pgs[i_start], &i_delta);
    assert_nc( i_sop<=seg->i_leaf_offset );

    if( detail_none ){
      while( i_sop<seg->i_leaf_offset ){
        if( pgs[i_sop]==0x00 ) i_sop++;
        if( pgs[i_sop]==0x00 ) i_sop++;
        i_start = i_sop;
        i_sop = i_start + __fts5_get_varint(&pgs[i_start], &i_delta);
      }

      i_next_off = i_sop;
      if( i_next_off<seg->i_endof_doclist && pgs[i_next_off]==0x00 ) i_next_off++;
      if( i_next_off<seg->i_endof_doclist && pgs[i_next_off]==0x00 ) i_next_off++;

    }else{
      int n_pos = 0;
      i_sop += FTS5_GET_VARINT32(&pgs[i_sop], n_pos);
      while( i_sop<seg->i_leaf_offset ){
        i_start = i_sop + (n_pos/2);
        i_sop = i_start + __fts5_get_varint(&pgs[i_start], &i_delta);
        i_sop += FTS5_GET_VARINT32(&pgs[i_sop], n_pos);
      }
      assert_nc( i_sop==seg->i_leaf_offset );
      i_next_off = i_sop + seg->n_pos;
    }
  }

  i_off = i_start;

  /* If the position-list for the entry being removed flows over past
  ** the end of this page, delete the portion of the position-list on the
  ** next page and beyond.
  **
  ** Set variable last_in_doclist to true if this entry happens 
  ** to be the last rowid in the doclist for its term.  */
  if( i_next_off>=i_pg_idx ){
    int pgno = seg->i_leaf_pgno+1;
    __fts5_secure_delete_overflow(p, seg->seg, pgno, &last_in_doclist);
    i_next_off = i_pg_idx;
  }

  if( seg->del==0 ){
    if( i_next_off!=i_pg_idx ){
      /* Loop through the page-footer. If i_next_off (offset of the
      ** entry following the one we are removing) is equal to the 
      ** offset of a key on this page, then the entry is the last 
      ** in its doclist. */
      int i_key_off = 0;
      for(i_idx=0; i_idx<n_idx; /* no-op */){
        u_int32_t i_val = 0;
        i_idx += FTS5_GET_VARINT32(&idx[i_idx], i_val);
        i_key_off += i_val;
        if( i_key_off==i_next_off ){
          last_in_doclist = 1;
        }
      }
    }

    /* If this is (a) the first rowid on a page and (b) is not followed by
    ** another position list on the same page, set the "first-rowid" field
    ** of the header to 0.  */
    if( __fts5_get_u16(&pgs[0])==i_start && (last_in_doclist || i_next_off==i_pg_idx) ){
      __fts5_put_u16(&pgs[0], 0);
    }
  }

  if( seg->del ){
    i_off += __fts5_put_varint(&pgs[i_off], i_delta);
    pgs[i_off++] = 0x01;
  }else if( last_in_doclist==0 ){
    if( i_next_off!=i_pg_idx ){
      u_int64_t i_next_delta = 0;
      i_next_off += __fts5_get_varint(&pgs[i_next_off], &i_next_delta);
      i_off += __fts5_put_varint(&pgs[i_off], i_delta + i_next_delta);
    }
  }else if( 
      seg->i_leaf_pgno==seg->i_term_leaf_pgno 
   && i_start==seg->i_term_leaf_offset 
  ){
    /* The entry being removed was the only position list in its
    ** doclist. Therefore the term needs to be removed as well. */
    int i_key = 0;
    int i_key_off = 0;

    /* Set i_key_off to the offset of the term that will be removed - the
    ** last offset in the footer that is not greater than i_start. */
    for(i_idx=0; i_idx<n_idx; i_key++){
      u_int32_t i_val = 0;
      i_idx += FTS5_GET_VARINT32(&idx[i_idx], i_val);
      if( (i_key_off+i_val)>(u_int32_t)i_start ) break;
      i_key_off += i_val;
    }
    assert_nc( i_key>=1 );

    /* Set i_del_key_off to the value of the footer entry to remove from 
    ** the page. */
    i_del_key_off = i_off = i_key_off;

    if( i_next_off!=i_pg_idx ){
      /* This is the only position-list associated with the term, and there
      ** is another term following it on this page. So the subsequent term
      ** needs to be moved to replace the term associated with the entry
      ** being removed. */
      u_int64_t n_prefix = 0;
      u_int64_t n_suffix = 0;
      u_int64_t n_prefix2 = 0;
      u_int64_t n_suffix2 = 0;

      i_del_key_off = i_next_off;
      i_next_off += __fts5_get_varint(&pgs[i_next_off], &n_prefix2);
      i_next_off += __fts5_get_varint(&pgs[i_next_off], &n_suffix2);

      if( i_key!=1 ){
        i_key_off += __fts5_get_varint(&pgs[i_key_off], &n_prefix);
      }
      i_key_off += __fts5_get_varint(&pgs[i_key_off], &n_suffix);

      n_prefix = MIN(n_prefix, n_prefix2);
      n_suffix = (n_prefix2 + n_suffix2) - n_prefix;

      if( (i_key_off+n_suffix)>(u_int64_t)i_pg_idx || (i_next_off+n_suffix2)>(u_int64_t)i_pg_idx ){
        FTS5_CORRUPT_IDX(p);
      }else{
        if( i_key!=1 ){
          i_off += __fts5_put_varint(&pgs[i_off], n_prefix);
        }
        i_off += __fts5_put_varint(&pgs[i_off], n_suffix);
        if( n_prefix2>(u_int64_t)seg->term.n ){
          FTS5_CORRUPT_IDX(p);
        }else if( n_prefix2>n_prefix ){
          memcpy(&pgs[i_off], &seg->term.p[n_prefix], n_prefix2-n_prefix);
          i_off += (n_prefix2-n_prefix);
        }
        memmove(&pgs[i_off], &pgs[i_next_off], n_suffix2);
        i_off += n_suffix2;
        i_next_off += n_suffix2;
      }
    }
  }else if( i_start==4 ){
    int i_pgno;

    assert_nc( seg->i_leaf_pgno>seg->i_term_leaf_pgno );
    /* The entry being removed may be the only position list in
    ** its doclist. */
    for(i_pgno=seg->i_leaf_pgno-1; i_pgno>seg->i_term_leaf_pgno; i_pgno-- ){
      fts5_data_t *pg = __fts5_leaf_read(p, FTS5_SEGMENT_ROWID(i_segid, i_pgno));
      int empty = (pg && pg->nn==4);
      __fts5_data_release(pg);
      if( empty==0 ) break;
    }

    if( i_pgno==seg->i_term_leaf_pgno ){
      int64_t i_id = FTS5_SEGMENT_ROWID(i_segid, seg->i_term_leaf_pgno);
      fts5_data_t *term = __fts5_leaf_read(p, i_id);
      if( term && term->sz_leaf==seg->i_term_leaf_offset ){
        u_int8_t *term_idx = &term->p[term->sz_leaf];
        int n_term_idx = term->nn - term->sz_leaf;
        int i_term_idx = 0;
        int64_t i_term_off = 0;

        while( 1 ){
          u_int32_t i_val = 0;
          int n_byte = FTS5_GET_VARINT32(&term_idx[i_term_idx], i_val);
          i_term_off += i_val;
          if( (i_term_idx+n_byte)>=n_term_idx ) break;
          i_term_idx += n_byte;
        }
        n_term_idx = i_term_idx;

        if( i_term_off>term->sz_leaf ){
          FTS5_CORRUPT_IDX(p);
        }else{
          memmove(&term->p[i_term_off], &term->p[term->sz_leaf], n_term_idx);
          __fts5_put_u16(&term->p[2], i_term_off);
          __fts5_data_write(p, i_id, term->p, i_term_off+n_term_idx);
          if( n_term_idx==0 ){
            __fts5_secure_delete_idx_entry(p, i_segid, seg->i_term_leaf_pgno);
          }
        }
      }
      __fts5_data_release(term);
    }
  }

  /* Assuming no error has occurred, this block does final edits to the
  ** leaf page before writing it back to disk. Input variables are:
  **
  **   n_pg: Total initial size of leaf page.
  **   i_pg_idx: Initial offset of page footer.
  **
  **   i_off: Offset to move data to
  **   i_next_off: Offset to move data from
  */
  if( p->rc==DBSQL_OK ){
    const int n_move = n_pg - i_next_off;     /* Number of bytes to move */
    int n_shift = i_next_off - i_off;         /* Distance to move them */

    int i_prev_key_out = 0;
    int i_key_in = 0;

    if( n_move>0 ){
      memmove(&pgs[i_off], &pgs[i_next_off], n_move);
    }
    i_pg_idx -= n_shift;
    n_pg = i_pg_idx;
    __fts5_put_u16(&pgs[2], i_pg_idx);

    for(i_idx=0; i_idx<n_idx; /* no-op */){
      u_int32_t i_val = 0;
      i_idx += FTS5_GET_VARINT32(&idx[i_idx], i_val);
      i_key_in += i_val;
      if( i_key_in!=i_del_key_off ){
        int i_key_out = (i_key_in - (i_key_in>i_off ? n_shift : 0));
        n_pg += __fts5_put_varint(&pgs[n_pg], i_key_out - i_prev_key_out);
        i_prev_key_out = i_key_out;
      }
    }

    if( i_pg_idx==n_pg && n_idx>0 && seg->i_leaf_pgno!=1 ){
      __fts5_secure_delete_idx_entry(p, i_segid, seg->i_leaf_pgno);
    }

    assert_nc( n_pg>4 || __fts5_get_u16(pgs)==0 );
    __fts5_data_write(p, FTS5_SEGMENT_ROWID(i_segid,seg->i_leaf_pgno), pgs, n_pg);
  }
  dbsql_free(idx);
}

/*
** This is called as part of flushing a delete to disk in 'secure-delete'
** mode. It edits the segments within the database described by argument
** p_struct to remove the entries for term term, rowid i_rowid.
**
** Return DBSQL_OK if successful, or an dbsql error code if an error
** has occurred. Any error code is also stored in the fts5_index_t handle.
*/
static int __fts5_flush_secure_delete(
  fts5_index_t *p,
  fts5_structure_t *p_struct,
  const char *term,
  int n_term,
  int64_t i_rowid
){
  const int f = FTS5INDEX_QUERY_SKIPHASH;
  fts5_iter_t *iter = 0;            /* Used to find term instance */

  /* If the version number has not been set to SECUREDELETE, do so now. */
  if( p->config->i_version!=FTS5_CURRENT_VERSION_SECUREDELETE ){
    fts5_config_t *config = p->config;
    dbsql_stmt_t *stmt = 0;
    __fts5_index_prepare_stmt(p, &stmt, dbsql_mprintf(
          "REPLACE INTO %Q.'%q_config' VALUES ('version', %d)",
          config->z_db, config->name, FTS5_CURRENT_VERSION_SECUREDELETE
    ));
    if( p->rc==DBSQL_OK ){
      int rc;
      dbsql_step(stmt);
      rc = dbsql_finalize(stmt);
      if( p->rc==DBSQL_OK ) p->rc = rc;
      config->i_cookie++;
      config->i_version = FTS5_CURRENT_VERSION_SECUREDELETE;
    }
  }

  __fts5_multi_iter_new(p, p_struct, f, 0, (const u_int8_t*)term, n_term, -1, 0, &iter);
  if( __fts5_multi_iter_eof(p, iter)==0 ){
    int64_t i_this = __fts5_multi_iter_rowid(iter);
    if( i_this<i_rowid ){
      __fts5_multi_iter_next_from(p, iter, i_rowid);
    }

    if( p->rc==DBSQL_OK 
     && __fts5_multi_iter_eof(p, iter)==0 
     && i_rowid==__fts5_multi_iter_rowid(iter)
    ){
      fts5_seg_iter_t *seg = &iter->segs[iter->firsts[1].i_first];
      __fts5_do_secure_delete(p, seg);
    }
  }

  __fts5_multi_iter_free(iter);
  return p->rc;
}


/*
** Flush the contents of in-memory hash table i_hash to a new level-0 
** segment on disk. Also update the corresponding structure record.
**
** If an error occurs, set the fts5_index_t.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void __fts5_flush_one_hash(fts5_index_t *p){
  fts5_hash_t *hash = p->hash;
  fts5_structure_t *p_struct;
  int i_segid;
  int pgno_last = 0;                 /* Last leaf page number in segment */

  /* Obtain a reference to the index structure and allocate a new segment-id
  ** for the new level-0 segment.  */
  p_struct = __fts5_structure_read(p);
  __fts5_structure_invalidate(p);

  if( __fts5_hash_is_empty(hash)==0 ){
    i_segid = __fts5_allocate_segid(p, p_struct);
    if( i_segid ){
      const int pgsz = p->config->pgsz;
      int detail = p->config->detail;
      int secure_delete = p->config->secure_delete;
      fts5_structure_segment_t *seg; /* New segment within p_struct */
      fts5_buffer_t *buf;           /* Buffer in which to assemble leaf page */
      fts5_buffer_t *pgidx;         /* Buffer in which to assemble pgidx */
  
      fts5_seg_writer_t writer;
      __fts5_write_init(p, &writer, i_segid);
  
      buf = &writer.writer.buf;
      pgidx = &writer.writer.pgidx;
  
      /* __fts5_write_init() should have initialized the buffers to (most likely)
      ** the maximum space required. */
      assert( p->rc || buf->n_space>=(pgsz + FTS5_DATA_PADDING) );
      assert( p->rc || pgidx->n_space>=(pgsz + FTS5_DATA_PADDING) );
  
      /* Begin scanning through hash table entries. This loop runs once for each
      ** term/doclist currently stored within the hash table. */
      if( p->rc==DBSQL_OK ){
        p->rc = __fts5_hash_scan_init(hash, 0, 0);
      }
      while( p->rc==DBSQL_OK && 0==__fts5_hash_scan_eof(hash) ){
        const char *term;        /* Buffer containing term */
        int n_term;                /* Size of term in bytes */
        const u_int8_t *doclist;       /* Pointer to doclist for this term */
        int n_doclist;             /* Size of doclist in bytes */
  
        /* Get the term and doclist for this entry. */
        __fts5_hash_scan_entry(hash, &term, &n_term, &doclist, &n_doclist);
        if( secure_delete==0 ){
          __fts5_write_append_term(p, &writer, n_term, (const u_int8_t*)term);
          if( p->rc!=DBSQL_OK ) break;
          assert( writer.first_rowid_in_page==0 );
        }
  
        if( !secure_delete && pgsz>=(buf->n + pgidx->n + n_doclist + 1) ){
          /* The entire doclist will fit on the current leaf. */
          FTS5_BUFFER_SAFE_APPEND_BLOB(buf, doclist, n_doclist);
        }else{
          int term_written = !secure_delete;
          int64_t i_rowid = 0;
          int64_t i_prev = 0;
          int i_off = 0;
  
          /* The entire doclist will not fit on this leaf. The following 
          ** loop iterates through the poslists that make up the current 
          ** doclist.  */
          while( p->rc==DBSQL_OK && i_off<n_doclist ){
            u_int64_t i_delta = 0;
            i_off += __fts5_get_varint(&doclist[i_off], &i_delta);
            i_rowid += i_delta;
  
            /* If in secure delete mode, and if this entry in the poslist is
            ** in fact a delete, then edit the existing segments directly
            ** using __fts5_flush_secure_delete().  */
            if( secure_delete ){
              if( detail==FTS5_DETAIL_NONE ){
                if( i_off<n_doclist && doclist[i_off]==0x00 
                 && !__fts5_flush_secure_delete(p, p_struct, term, n_term, i_rowid)
                ){
                  i_off++;
                  if( i_off<n_doclist && doclist[i_off]==0x00 ){
                    i_off++;
                    n_doclist = 0;
                  }else{
                    continue;
                  }
                }
              }else if( (doclist[i_off] & 0x01) 
                && !__fts5_flush_secure_delete(p, p_struct, term, n_term, i_rowid)
              ){
                if( p->rc!=DBSQL_OK || doclist[i_off]==0x01 ){
                  i_off++;
                  continue;
                }
              }
            }
  
            if( p->rc==DBSQL_OK && term_written==0 ){
              __fts5_write_append_term(p, &writer, n_term, (const u_int8_t*)term);
              term_written = 1;
              assert( p->rc!=DBSQL_OK || writer.first_rowid_in_page==0 );
            }
            
            if( writer.first_rowid_in_page ){
              __fts5_put_u16(&buf->p[0], (u_int16_t)buf->n);   /* first rowid on page */
              buf->n += __fts5_put_varint(&buf->p[buf->n], i_rowid);
              writer.first_rowid_in_page = 0;
              __fts5_write_dlidx_append(p, &writer, i_rowid);
            }else{
              u_int64_t i_rowid_delta = (u_int64_t)i_rowid - (u_int64_t)i_prev;
              buf->n += __fts5_put_varint(&buf->p[buf->n], i_rowid_delta);
            }
            if( p->rc!=DBSQL_OK ) break;
            assert( buf->n<=buf->n_space );
            i_prev = i_rowid;
  
            if( detail==FTS5_DETAIL_NONE ){
              if( i_off<n_doclist && doclist[i_off]==0 ){
                buf->p[buf->n++] = 0;
                i_off++;
                if( i_off<n_doclist && doclist[i_off]==0 ){
                  buf->p[buf->n++] = 0;
                  i_off++;
                }
              }
              if( (buf->n + pgidx->n)>=pgsz ){
                __fts5_write_flush_leaf(p, &writer);
              }
            }else{
              int del = 0;
              int n_pos = 0;
              int n_copy = __fts5_get_poslist_size(&doclist[i_off], &n_pos, &del);
              if( del && secure_delete ){
                FTS5_BUFFER_APPEND_VARINT(&p->rc, buf, n_pos*2);
                i_off += n_copy;
                n_copy = n_pos;
              }else{
                n_copy += n_pos;
              }
              if( (buf->n + pgidx->n + n_copy) <= pgsz ){
                /* The entire poslist will fit on the current leaf. So copy
                ** it in one go. */
                FTS5_BUFFER_SAFE_APPEND_BLOB(buf, &doclist[i_off], n_copy);
              }else{
                /* The entire poslist will not fit on this leaf. So it needs
                ** to be broken into sections. The only qualification being
                ** that each varint must be stored contiguously.  */
                const u_int8_t *poslist = &doclist[i_off];
                int i_pos = 0;
                while( p->rc==DBSQL_OK ){
                  int n_space = pgsz - buf->n - pgidx->n;
                  int n = 0;
                  if( (n_copy - i_pos)<=n_space ){
                    n = n_copy - i_pos;
                  }else{
                    n = __fts5_poslist_prefix(&poslist[i_pos], n_space);
                  }
                  assert( n>0 );
                  FTS5_BUFFER_SAFE_APPEND_BLOB(buf, &poslist[i_pos], n);
                  i_pos += n;
                  if( (buf->n + pgidx->n)>=pgsz ){
                    __fts5_write_flush_leaf(p, &writer);
                  }
                  if( i_pos>=n_copy ) break;
                }
              }
              i_off += n_copy;
            }
          }
        }
  
        /* TODO2: Doclist terminator written here. */
        /* buf->p[buf->n++] = '\0'; */
        assert( buf->n<=buf->n_space );
        if( p->rc==DBSQL_OK ) __fts5_hash_scan_next(hash);
      }
      __fts5_write_finish(p, &writer, &pgno_last);
  
      assert( p->rc!=DBSQL_OK || secure_delete || pgno_last>0 );
      if( pgno_last>0 ){
        /* Update the fts5_structure_t. It is written back to the database by the
        ** __fts5_structure_release_local() call below.  */
        if( p_struct->n_level==0 ){
          __fts5_structure_add_level(&p->rc, &p_struct);
        }
        __fts5_structure_extend_level(&p->rc, p_struct, 0, 1, 0);
        if( p->rc==DBSQL_OK ){
          seg = &p_struct->levels[0].segs[ p_struct->levels[0].n_seg++ ];
          seg->i_segid = i_segid;
          seg->pgno_first = 1;
          seg->pgno_last = pgno_last;
          if( p_struct->n_origin_cntr>0 ){
            seg->i_origin1 = p_struct->n_origin_cntr;
            seg->i_origin2 = p_struct->n_origin_cntr;
            seg->n_entry = p->n_pending_row;
            p_struct->n_origin_cntr++;
          }
          p_struct->n_segment++;
        }
        __fts5_structure_promote(p, 0, p_struct);
      }
    }
  }

  __fts5_index_automerge(p, &p_struct, pgno_last + p->n_contentless_delete);
  __fts5_index_crisismerge(p, &p_struct);
  __fts5_structure_write(p, p_struct);
  __fts5_structure_release_local(p_struct);
}

/*
** Flush any data stored in the in-memory hash tables to the database.
*/
static void __fts5_index_flush(fts5_index_t *p){
  /* Unless it is empty, flush the hash table to disk */
  if( p->flush_rc ){
    p->rc = p->flush_rc;
    return;
  }
  if( p->n_pending_data || p->n_contentless_delete ){
    assert( p->hash );
    __fts5_flush_one_hash(p);
    if( p->rc==DBSQL_OK ){
      __fts5_hash_clear(p->hash);
      p->n_pending_data = 0;
      p->n_pending_row = 0;
      p->n_contentless_delete = 0;
    }else if( p->n_pending_data || p->n_contentless_delete ){
      p->flush_rc = p->rc;
    }
  }
}

static fts5_structure_t *__fts5_index_optimize_struct(
  fts5_index_t *p, 
  fts5_structure_t *p_struct
){
  fts5_structure_t *p_new = 0;
  int64_t n_byte = SZ_FTS5STRUCTURE(1);
  int n_seg = p_struct->n_segment;
  int i;

  /* Figure out if this structure requires optimization. A structure does
  ** not require optimization if either:
  **
  **  1. it consists of fewer than two segments, or 
  **  2. all segments are on the same level, or
  **  3. all segments except one are currently inputs to a merge operation.
  **
  ** In the first case, if there are no tombstone hash pages, return NULL. In
  ** the second, increment the ref-count on *p_struct and return a copy of the
  ** pointer to it.
  */
  if( n_seg==0 ) return 0;
  for(i=0; i<p_struct->n_level; i++){
    int n_this = p_struct->levels[i].n_seg;
    int n_merge = p_struct->levels[i].n_merge;
    if( n_this>0 && (n_this==n_seg || (n_this==n_seg-1 && n_merge==n_this)) ){
      if( n_seg==1 && n_this==1 && p_struct->levels[i].segs[0].n_pg_tombstone==0 ){
        return 0;
      }
      __fts5_structure_ref_local(p_struct);
      return p_struct;
    }
    assert( p_struct->levels[i].n_merge<=n_this );
  }

  n_byte += (((int64_t)p_struct->n_level)+1) * sizeof(fts5_structure_level_t);
  assert( n_byte==(int64_t)SZ_FTS5STRUCTURE(p_struct->n_level+2) );
  p_new = (fts5_structure_t*)__fts5_malloc_zero(&p->rc, n_byte);

  if( p_new ){
    fts5_structure_level_t *lvl;
    n_byte = n_seg * sizeof(fts5_structure_segment_t);
    p_new->n_level = MIN(p_struct->n_level+1, FTS5_MAX_LEVEL);
    p_new->n_ref = 1;
    p_new->n_write_counter = p_struct->n_write_counter;
    p_new->n_origin_cntr = p_struct->n_origin_cntr;
    lvl = &p_new->levels[p_new->n_level-1];
    lvl->segs = (fts5_structure_segment_t*)__fts5_malloc_zero(&p->rc, n_byte);
    if( lvl->segs ){
      int i_lvl, i_seg;
      int i_seg_out = 0;
      /* Iterate through all segments, from oldest to newest. Add them to
      ** the new Fts5Level object so that lvl->segs[0] is the oldest
      ** segment in the data structure.  */
      for(i_lvl=p_struct->n_level-1; i_lvl>=0; i_lvl--){
        for(i_seg=0; i_seg<p_struct->levels[i_lvl].n_seg; i_seg++){
          lvl->segs[i_seg_out] = p_struct->levels[i_lvl].segs[i_seg];
          i_seg_out++;
        }
      }
      p_new->n_segment = lvl->n_seg = n_seg;
    }else{
      dbsql_free(p_new);
      p_new = 0;
    }
  }

  return p_new;
}

int __fts5_index_optimize(fts5_index_t *p){
  fts5_structure_t *p_struct;
  fts5_structure_t *p_new = 0;

  assert( p->rc==DBSQL_OK );
  __fts5_index_flush(p);
  assert( p->rc!=DBSQL_OK || p->n_contentless_delete==0 );
  p_struct = __fts5_structure_read(p);
  assert( p->rc!=DBSQL_OK || p_struct!=0 );
  __fts5_structure_invalidate(p);

  if( p_struct ){
    p_new = __fts5_index_optimize_struct(p, p_struct);
  }
  __fts5_structure_release_local(p_struct);

  assert( p_new==0 || p_new->n_segment>0 );
  if( p_new ){
    int i_lvl;
    for(i_lvl=0; p_new->levels[i_lvl].n_seg==0; i_lvl++){}
    while( p->rc==DBSQL_OK && p_new->levels[i_lvl].n_seg>0 ){
      int n_rem = FTS5_OPT_WORK_UNIT;
      __fts5_index_merge_level(p, &p_new, i_lvl, &n_rem);
    }

    __fts5_structure_write(p, p_new);
    __fts5_structure_release_local(p_new);
  }

  return __fts5_index_return(p); 
}

/*
** This is called to implement the special "VALUES('merge', $n_merge)"
** INSERT command.
*/
int __fts5_index_merge(fts5_index_t *p, int n_merge){
  fts5_structure_t *p_struct = 0;

  __fts5_index_flush(p);
  p_struct = __fts5_structure_read(p);
  if( p_struct ){
    int n_min = p->config->n_usermerge;
    __fts5_structure_invalidate(p);
    if( n_merge<0 ){
      fts5_structure_t *p_new = __fts5_index_optimize_struct(p, p_struct);
      __fts5_structure_release_local(p_struct);
      p_struct = p_new;
      n_min = 1;
      n_merge = (n_merge==SMALLEST_INT32 ? LARGEST_INT32 : (n_merge*-1));
    }
    if( p_struct && p_struct->n_level ){
      if( __fts5_index_merge_local(p, &p_struct, n_merge, n_min) ){
        __fts5_structure_write(p, p_struct);
      }
    }
    __fts5_structure_release_local(p_struct);
  }
  return __fts5_index_return(p);
}

static void __fts5_append_rowid(
  fts5_index_t *p,
  u_int64_t i_delta,
  fts5_iter_t *unused,
  fts5_buffer_t *buf
){
  UNUSED_PARAM(unused);
  FTS5_BUFFER_APPEND_VARINT(&p->rc, buf, i_delta);
}

static void __fts5_append_poslist(
  fts5_index_t *p,
  u_int64_t i_delta,
  fts5_iter_t *multi,
  fts5_buffer_t *buf
){
  int n_data = multi->base.n_data;
  int n_byte = n_data + 9 + 9 + FTS5_DATA_ZERO_PADDING;
  assert( n_data>0 );
  if( p->rc==DBSQL_OK && 0==FTS5_BUFFER_GROW(&p->rc, buf, n_byte) ){
    FTS5_BUFFER_SAFE_APPEND_VARINT(buf, i_delta);
    FTS5_BUFFER_SAFE_APPEND_VARINT(buf, n_data*2);
    FTS5_BUFFER_SAFE_APPEND_BLOB(buf, multi->base.data, n_data);
    memset(&buf->p[buf->n], 0, FTS5_DATA_ZERO_PADDING);
  }
}


static void __fts5_doclist_iter_next(fts5_doclist_iter_t *iter){
  u_int8_t *p = iter->poslists + iter->n_size + iter->n_poslist;

  assert( iter->poslists || (p==0 && iter->poslists==0) );
  if( p>=iter->eofs ){
    iter->poslists = 0;
  }else{
    int64_t i_delta;

    p += __fts5_get_varint(p, (u_int64_t*)&i_delta);
    iter->i_rowid += i_delta;

    /* Read position list size */
    if( p[0] & 0x80 ){
      int n_pos;
      iter->n_size = FTS5_GET_VARINT32(p, n_pos);
      iter->n_poslist = (n_pos>>1);
    }else{
      iter->n_poslist = ((int)(p[0])) >> 1;
      iter->n_size = 1;
    }

    iter->poslists = p;
    if( &iter->poslists[iter->n_poslist]>iter->eofs ){
      iter->poslists = 0;
    }
  }
}

static void __fts5_doclist_iter_init(
  fts5_buffer_t *buf, 
  fts5_doclist_iter_t *iter
){
  memset(iter, 0, sizeof(*iter));
  if( buf->n>0 ){
    iter->poslists = buf->p;
    iter->eofs = &buf->p[buf->n];
    __fts5_doclist_iter_next(iter);
  }
}

#if 0
/*
** Append a doclist to buffer buf.
**
** This function assumes that space within the buffer has already been
** allocated.
*/
static void FTS5_MERGE_APPEND_DOCID(
  fts5_buffer_t *buf,               /* Buffer to write to */
  int64_t *pi_last_rowid,               /* IN/OUT: Previous rowid written (if any) */
  int64_t i_rowid                      /* Rowid to append */
){
  assert( buf->n!=0 || (*pi_last_rowid)==0 );
  FTS5_BUFFER_SAFE_APPEND_VARINT(buf, i_rowid - *pi_last_rowid);
  *pi_last_rowid = i_rowid;
}
#endif

#define FTS5_MERGE_APPEND_DOCID(buf, i_last_rowid, i_rowid) {                 \
  assert( (buf)->n!=0 || (i_last_rowid)==0 );                             \
  FTS5_BUFFER_SAFE_APPEND_VARINT((buf), (u_int64_t)(i_rowid) - (u_int64_t)(i_last_rowid)); \
  (i_last_rowid) = (i_rowid);                                               \
}

/*
** Swap the contents of buffer *p1 with that of *p2.
*/
static void __fts5_buffer_swap(fts5_buffer_t *p1, fts5_buffer_t *p2){
  fts5_buffer_t tmp = *p1;
  *p1 = *p2;
  *p2 = tmp;
}

static void __fts5_next_rowid(fts5_buffer_t *buf, int *pi_off, int64_t *pi_rowid){
  int i = *pi_off;
  if( i>=buf->n ){
    *pi_off = -1;
  }else{
    u_int64_t i_val;
    *pi_off = i + __fts5_get_varint(&buf->p[i], &i_val);
    *pi_rowid += i_val;
  }
}

/*
** This is the equivalent of __fts5_merge_prefix_lists() for detail=none mode.
** In this case the buffers consist of a delta-encoded list of rowids only.
*/
static void __fts5_merge_rowid_lists(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_buffer_t *p1,                 /* First list to merge */
  int n_buf,                       /* Number of entries in bufs[] */
  fts5_buffer_t *bufs                /* Array of other lists to merge into p1 */
){
  int i1 = 0;
  int i2 = 0;
  int64_t i_rowid1 = 0;
  int64_t i_rowid2 = 0;
  int64_t i_out = 0;
  fts5_buffer_t *p2 = &bufs[0];
  fts5_buffer_t out;

  (void)n_buf;
  memset(&out, 0, sizeof(out));
  assert( n_buf==1 );
  __fts5_buffer_size(&p->rc, &out, p1->n + p2->n);
  if( p->rc ) return;

  __fts5_next_rowid(p1, &i1, &i_rowid1);
  __fts5_next_rowid(p2, &i2, &i_rowid2);
  while( i1>=0 || i2>=0 ){
    if( i1>=0 && (i2<0 || i_rowid1<i_rowid2) ){
      assert( i_out==0 || i_rowid1>i_out );
      FTS5_BUFFER_SAFE_APPEND_VARINT(&out, i_rowid1 - i_out);
      i_out = i_rowid1;
      __fts5_next_rowid(p1, &i1, &i_rowid1);
    }else{
      assert( i_out==0 || i_rowid2>i_out );
      FTS5_BUFFER_SAFE_APPEND_VARINT(&out, i_rowid2 - i_out);
      i_out = i_rowid2;
      if( i1>=0 && i_rowid1==i_rowid2 ){
        __fts5_next_rowid(p1, &i1, &i_rowid1);
      }
      __fts5_next_rowid(p2, &i2, &i_rowid2);
    }
  }

  __fts5_buffer_swap(&out, p1);
  FTS5_BUFFER_FREE(&out);
}

typedef struct prefix_merger_t prefix_merger_t;
struct prefix_merger_t {
  fts5_doclist_iter_t iter;           /* Doclist iterator */
  int64_t i_pos;                       /* For iterating through a position list */
  int i_off;
  u_int8_t *poses;
  prefix_merger_t *next;            /* Next in docid/poslist order */
};

static void __fts5_prefix_merger_insert_by_rowid(
  prefix_merger_t **head, 
  prefix_merger_t *p
){
  if( p->iter.poslists ){
    prefix_merger_t **pp = head;
    while( *pp && p->iter.i_rowid>(*pp)->iter.i_rowid ){
      pp = &(*pp)->next;
    }
    p->next = *pp;
    *pp = p;
  }
}

static void __fts5_prefix_merger_insert_by_position(
  prefix_merger_t **head, 
  prefix_merger_t *p
){
  if( p->i_pos>=0 ){
    prefix_merger_t **pp = head;
    while( *pp && p->i_pos>(*pp)->i_pos ){
      pp = &(*pp)->next;
    }
    p->next = *pp;
    *pp = p;
  }
}


/*
** Array bufs[] contains n_buf doclists. These are all merged in with the
** doclist in buffer p1.
*/
static void __fts5_merge_prefix_lists(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_buffer_t *p1,                 /* First list to merge */
  int n_buf,                       /* Number of buffers in array bufs[] */
  fts5_buffer_t *bufs                /* Other lists to merge in */ 
){
#define FTS5_PREFIX_MERGER_NEXT_POSITION(p) \
  __fts5_poslist_next64((p)->poses,(p)->iter.n_poslist,&(p)->i_off,&(p)->i_pos)
#define FTS5_MERGE_NLIST 16
  prefix_merger_t mergers[FTS5_MERGE_NLIST];
  prefix_merger_t *head = 0;
  int i;
  int n_out = 0;
  fts5_buffer_t out = {0, 0, 0};
  fts5_buffer_t tmp = {0, 0, 0};
  int64_t i_last_rowid = 0;

  /* Initialize a doclist-iterator for each input buffer. Arrange them in
  ** a linked-list starting at head in ascending order of rowid. Avoid
  ** linking any iterators already at EOF into the linked list at all. */ 
  assert( n_buf+1<=(int)(sizeof(mergers)/sizeof(mergers[0])) );
  memset(mergers, 0, sizeof(prefix_merger_t)*(n_buf+1));
  head = &mergers[n_buf];
  __fts5_doclist_iter_init(p1, &head->iter);
  for(i=0; i<n_buf; i++){
    __fts5_doclist_iter_init(&bufs[i], &mergers[i].iter);
    __fts5_prefix_merger_insert_by_rowid(&head, &mergers[i]);
    n_out += bufs[i].n;
  }
  if( n_out==0 ) return;
  n_out += p1->n + 9 + 10*n_buf;

  /* The maximum size of the output is equal to the sum of the
  ** input sizes + 1 varint (9 bytes). The extra varint is because if the
  ** first rowid in one input is a large negative number, and the first in
  ** the other a non-negative number, the delta for the non-negative
  ** number will be larger on disk than the literal integer value
  ** was.  
  **
  ** Or, if the input position-lists are corrupt, then the output might
  ** include up to (n_buf+1) extra 10-byte positions created by interpreting -1
  ** (the value __poslist_next64() uses for EOF) as a position and appending
  ** it to the output. This can happen at most once for each input 
  ** position-list, hence (n_buf+1) 10 byte paddings.  */
  if( __fts5_buffer_size(&p->rc, &out, n_out) ) return;

  while( head ){
    FTS5_MERGE_APPEND_DOCID(&out, i_last_rowid, head->iter.i_rowid);

    if( head->next && i_last_rowid==head->next->iter.i_rowid ){
      /* Merge data from two or more poslists */
      int64_t i_prev = 0;
      int n_tmp = FTS5_DATA_ZERO_PADDING;
      int n_merge = 0;
      prefix_merger_t *save = head;
      prefix_merger_t *p_this = 0;
      int n_tail = 0;

      head = 0;
      while( save && save->iter.i_rowid==i_last_rowid ){
        prefix_merger_t *next = save->next;
        save->i_off = 0;
        save->i_pos = 0;
        save->poses = &save->iter.poslists[save->iter.n_size];
        FTS5_PREFIX_MERGER_NEXT_POSITION(save);
        n_tmp += save->iter.n_poslist + 10;
        n_merge++;
        __fts5_prefix_merger_insert_by_position(&head, save);
        save = next;
      }

      if( head==0 || head->next==0 ){
        FTS5_CORRUPT_IDX(p);
        break;
      }

      /* See the earlier comment in this function for an explanation of why
      ** corrupt input position lists might cause the output to consume
      ** at most n_merge*10 bytes of unexpected space. */
      if( __fts5_buffer_size(&p->rc, &tmp, n_tmp+n_merge*10) ){
        break;
      }
      FTS5_BUFFER_ZERO(&tmp);

      p_this = head;
      head = p_this->next;
      __fts5_poslist_safe_append(&tmp, &i_prev, p_this->i_pos);
      FTS5_PREFIX_MERGER_NEXT_POSITION(p_this);
      __fts5_prefix_merger_insert_by_position(&head, p_this);

      while( head->next ){
        p_this = head;
        if( p_this->i_pos!=i_prev ){
          __fts5_poslist_safe_append(&tmp, &i_prev, p_this->i_pos);
        }
        FTS5_PREFIX_MERGER_NEXT_POSITION(p_this);
        head = p_this->next;
        __fts5_prefix_merger_insert_by_position(&head, p_this);
      }

      if( head->i_pos!=i_prev ){
        __fts5_poslist_safe_append(&tmp, &i_prev, head->i_pos);
      }
      n_tail = head->iter.n_poslist - head->i_off;

      /* WRITEPOSLISTSIZE */
      assert_nc( tmp.n+n_tail<=n_tmp );
      assert( tmp.n+n_tail<=n_tmp+n_merge*10 );
      if( tmp.n+n_tail>n_tmp-FTS5_DATA_ZERO_PADDING ){
        if( p->rc==DBSQL_OK ) FTS5_CORRUPT_IDX(p);
        break;
      }
      FTS5_BUFFER_SAFE_APPEND_VARINT(&out, (tmp.n+n_tail) * 2);
      FTS5_BUFFER_SAFE_APPEND_BLOB(&out, tmp.p, tmp.n);
      if( n_tail>0 ){
        FTS5_BUFFER_SAFE_APPEND_BLOB(&out, &head->poses[head->i_off], n_tail);
      }

      head = save;
      for(i=0; i<n_buf+1; i++){
        prefix_merger_t *x = &mergers[i];
        if( x->iter.poslists && x->iter.i_rowid==i_last_rowid ){
          __fts5_doclist_iter_next(&x->iter);
          __fts5_prefix_merger_insert_by_rowid(&head, x);
        }
      }

    }else{
      /* Copy poslist from head to output */
      prefix_merger_t *p_this = head;
      fts5_doclist_iter_t *p_i = &p_this->iter;
      FTS5_BUFFER_SAFE_APPEND_BLOB(&out, p_i->poslists, p_i->n_poslist+p_i->n_size);
      __fts5_doclist_iter_next(p_i);
      head = p_this->next;
      __fts5_prefix_merger_insert_by_rowid(&head, p_this);
    }
  }

  FTS5_BUFFER_FREE(p1);
  FTS5_BUFFER_FREE(&tmp);
  memset(&out.p[out.n], 0, FTS5_DATA_ZERO_PADDING);
  *p1 = out;
}


/*
** Iterate through a range of entries in the FTS index, invoking the visit
** callback for each of them.
**
** Parameter token points to an n_token buffer containing an FTS index term
** (i.e. a document term with the preceding 1 byte index identifier -
** FTS5_MAIN_PREFIX or similar). If prefix is true, then the call visits
** all entries for terms that have token/n_token as a prefix. If prefix
** is false, then only entries with token/n_token as the entire key are
** visited. 
**
** If the current table is a tokendata=1 table, then if prefix is true then
** each index term is treated separately. However, if prefix is false, then
** all index terms corresponding to token/n_token are collapsed into a single
** term before the callback is invoked.
**
** The callback invoked for each entry visited is specified by paramter visit.
** Each time it is invoked, it is passed a pointer to the fts5_index_t object,
** a copy of the 7th paramter to this function (ctx) and a pointer to the
** iterator that indicates the current entry. If the current entry is the
** first with a new term (i.e. different from that of the previous entry,
** including the very first term), then the final two parameters are passed
** a pointer to the term and its size in bytes, respectively. If the current
** entry is not the first associated with its term, these two parameters
** are passed 0.
**
** If parameter colset is not NULL, then it is used to filter entries before
** the callback is invoked.
*/
static int __fts5_visit_entries(
  fts5_index_t *p,                   /* Fts5 index object */
  fts5_colset_t *colset,            /* Columns filter to apply, or NULL */
  u_int8_t *token,                     /* Buffer containing token */
  int n_token,                     /* Size of buffer token in bytes */
  int prefix,                    /* True for a prefix scan */
  void (*visit)(fts5_index_t*, void *ctx, fts5_iter_t *iter, const u_int8_t*, int),
  void *ctx                      /* Passed as second argument to visit() */
){
  const int flags = (prefix ? FTS5INDEX_QUERY_SCAN : 0)
                  | FTS5INDEX_QUERY_SKIPEMPTY 
                  | FTS5INDEX_QUERY_NOOUTPUT;
  fts5_iter_t *p1 = 0;     /* Iterator used to gather data from index */
  int new_term = 1;
  fts5_structure_t *p_struct = __fts5_structure_read(p);

  __fts5_multi_iter_new(p, p_struct, flags, colset, token, n_token, -1, 0, &p1);
  __fts5_iter_set_output_cb(&p->rc, p1);
  for( /* no-op */ ;
      __fts5_multi_iter_eof(p, p1)==0;
      __fts5_multi_iter_next2(p, p1, &new_term)
  ){
    fts5_seg_iter_t *seg = &p1->segs[ p1->firsts[1].i_first ];
    int n_new = 0;
    const u_int8_t *p_new = 0;

    p1->set_outputs(p1, seg);
    if( p->rc ) break;

    if( new_term ){
      n_new = seg->term.n;
      p_new = seg->term.p;
      if( n_new<n_token || memcmp(token, p_new, n_token) ) break;
    }

    visit(p, ctx, p1, p_new, n_new);
  }
  __fts5_multi_iter_free(p1);

  __fts5_structure_release_local(p_struct);
  return p->rc;
}


/*
** Usually, a tokendata=1 iterator (struct fts5_token_data_iter_t) accumulates an
** array of these for each row it visits (so all i_rowid fields are the same).
** Or, for an iterator used by an "ORDER BY rank" query, it accumulates an
** array of these for the entire query (in which case i_rowid fields may take
** a variety of values).
**
** Each instance in the array indicates the iterator (and therefore term)
** associated with position i_pos of rowid i_rowid. This is used by the
** inst_token() API.
**
** i_rowid:
**   Rowid for the current entry.
**
** i_pos:
**   Position of current entry within row. In the usual ((i_col<<32)+i_off)
**   format (e.g. see macros FTS5_POS2COLUMN() and FTS5_POS2OFFSET()).
**
** i_iter:
**   If the fts5_token_data_iter_t iterator that the entry is part of is
**   actually an iterator (i.e. with n_iter>0, not just a container for
**   fts5_token_data_map_t structures), then this variable is an index into
**   the iters[] array. The corresponding term is that which the iterator
**   at iters[i_iter] currently points to.
**
**   Or, if the fts5_token_data_iter_t iterator is just a container object
**   (n_iter==0), then i_iter is an index into the term.p[] buffer where
**   the term is stored.
**
** n_byte:
**   In the case where i_iter is an index into term.p[], this variable
**   is the size of the term in bytes. If i_iter is an index into iters[],
**   this variable is unused.
*/
struct fts5_token_data_map_t {
  int64_t i_rowid;                     /* Row this token is located in */
  int64_t i_pos;                       /* Position of token */
  int i_iter;                      /* Iterator token was read from */
  int n_byte;                      /* Length of token in bytes (or 0) */
};

/*
** An object used to supplement fts5_iter_t for tokendata=1 iterators.
**
** This object serves two purposes. The first is as a container for an array
** of fts5_token_data_map_t structures, which are used to find the token required
** when the inst_token() API is used. This is done by the n_map_alloc, n_map and
** maps[] variables.
*/
struct fts5_token_data_iter_t {
  int64_t n_map_alloc;                  /* Allocated size of maps[] in entries */
  int64_t n_map;                       /* Number of valid entries in maps[] */
  fts5_token_data_map_t *maps;         /* Array of (rowid+pos -> token) mappings */

  /* The following are used for prefix-queries only. */
  fts5_buffer_t terms;

  /* The following are used for other full-token tokendata queries only. */
  int64_t n_iter;
  int64_t n_iter_alloc;
  fts5_poslist_reader_t *poslist_readers;
  int *poslist_to_iters;
  fts5_iter_t *iters[FLEXARRAY];
};

/* Size in bytes of an fts5_token_data_iter_t object holding up to N iterators */
#define SZ_FTS5TOKENDATAITER(N) \
    (offsetof(fts5_token_data_iter_t,iters) + (N)*sizeof(fts5_iter_t))

/*
** The two input arrays - a1[] and a2[] - are in sorted order. This function
** merges the two arrays together and writes the result to output array 
** outs[]. outs[] is guaranteed to be large enough to hold the result.
**
** Duplicate entries are copied into the output. So the size of the output
** array is always (n1+n2) entries.
*/
static void __fts5_tokendata_merge(
  fts5_token_data_map_t *a1, int n1,   /* Input array 1 */
  fts5_token_data_map_t *a2, int n2,   /* Input array 2 */
  fts5_token_data_map_t *outs          /* Output array */
){
  int i1 = 0;
  int i2 = 0;

  assert( n1>=0 && n2>=0 );
  while( i1<n1 || i2<n2 ){
    fts5_token_data_map_t *out = &outs[i1+i2];
    if( i2>=n2 || (i1<n1 && (
        a1[i1].i_rowid<a2[i2].i_rowid
     || (a1[i1].i_rowid==a2[i2].i_rowid && a1[i1].i_pos<=a2[i2].i_pos)
    ))){
      memcpy(out, &a1[i1], sizeof(fts5_token_data_map_t));
      i1++;
    }else{
      memcpy(out, &a2[i2], sizeof(fts5_token_data_map_t));
      i2++;
    }
  }
}


/*
** Append a mapping to the token-map belonging to object t.
*/
static void __fts5_tokendata_iter_append_map(
  fts5_index_t *p, 
  fts5_token_data_iter_t *t, 
  int i_iter,
  int n_byte,
  int64_t i_rowid, 
  int64_t i_pos
){
  if( p->rc==DBSQL_OK ){
    if( t->n_map==t->n_map_alloc ){
      int64_t n_new = t->n_map_alloc ? t->n_map_alloc*2 : 64;
      int64_t n_alloc = n_new * sizeof(fts5_token_data_map_t);
      fts5_token_data_map_t *news;

      news = (fts5_token_data_map_t*)dbsql_realloc64(t->maps, n_alloc);
      if( news==0 ){
        p->rc = DBSQL_NOMEM;
        return;
      }

      t->maps = news;
      t->n_map_alloc = n_new;
    }

    t->maps[t->n_map].i_rowid = i_rowid;
    t->maps[t->n_map].i_pos = i_pos;
    t->maps[t->n_map].i_iter = i_iter;
    t->maps[t->n_map].n_byte = n_byte;
    t->n_map++;
  }
}

/*
** Sort the contents of the t->maps[] array.
**
** The sorting algorithm requires a malloc(). If this fails, an error code
** is left in fts5_index_t.rc before returning.
*/
static void __fts5_tokendata_iter_sort_map(fts5_index_t *p, fts5_token_data_iter_t *t){
  fts5_token_data_map_t *tmps = 0;
  int64_t n_byte = t->n_map * sizeof(fts5_token_data_map_t);

  tmps = (fts5_token_data_map_t*)__fts5_malloc_zero(&p->rc, n_byte);
  if( tmps ){
    fts5_token_data_map_t *a1 = t->maps;
    fts5_token_data_map_t *a2 = tmps;
    int64_t n_half;

    for(n_half=1; n_half<t->n_map; n_half=n_half*2){
      int i1;
      for(i1=0; i1<t->n_map; i1+=(n_half*2)){
        int n1 = MIN(n_half, t->n_map-i1);
        int n2 = MIN(n_half, t->n_map-i1-n1);
        __fts5_tokendata_merge(&a1[i1], n1, &a1[i1+n1], n2, &a2[i1]);
      }
      SWAPVAL(fts5_token_data_map_t*, a1, a2);
    }

    if( a1!=t->maps ){
      memcpy(t->maps, a1, t->n_map*sizeof(fts5_token_data_map_t));
    }
    dbsql_free(tmps);

#ifdef DBSQL_DEBUG
    {
      int ii;
      for(ii=1; ii<t->n_map; ii++){
        fts5_token_data_map_t *p1 = &t->maps[ii-1];
        fts5_token_data_map_t *p2 = &t->maps[ii];
        assert( p1->i_rowid<p2->i_rowid 
             || (p1->i_rowid==p2->i_rowid && p1->i_pos<=p2->i_pos)
        );
      }
    }
#endif
  }
}

/*
** Delete an fts5_token_data_iter_t structure and its contents.
*/
static void __fts5_tokendata_iter_delete(fts5_token_data_iter_t *set){
  if( set ){
    int ii;
    for(ii=0; ii<set->n_iter; ii++){
      __fts5_multi_iter_free(set->iters[ii]);
    }
    FTS5_BUFFER_FREE(&set->terms);
    dbsql_free(set->poslist_readers);
    dbsql_free(set->maps);
    dbsql_free(set);
  }
}


/*
** __fts5_visit_entries() context object used by __fts5_setup_prefix_iter_tokendata()
** to pass data to __prefix_iter_setup_tokendata_cb().
*/
typedef struct tokendata_setup_ctx_t tokendata_setup_ctx_t;
struct tokendata_setup_ctx_t {
  fts5_token_data_iter_t *t;          /* Object being populated with mappings */
  int i_term_off;                   /* Offset of current term in terms.p[] */
  int n_term_byte;                  /* Size of current term in bytes */
};

/*
** __fts5_visit_entries() callback used by __fts5_setup_prefix_iter_tokendata(). This
** callback adds an entry to the fts5_token_data_iter_t.maps[] array for each
** position in the current position-list. It doesn't matter that some of
** these may be out of order - they will be sorted later.
*/
static void __prefix_iter_setup_tokendata_cb(
  fts5_index_t *p, 
  void *ctx, 
  fts5_iter_t *p1, 
  const u_int8_t *p_new,
  int n_new
){
  tokendata_setup_ctx_t *setup = (tokendata_setup_ctx_t*)ctx;
  int i_pos_off = 0;
  int64_t i_pos = 0;

  if( p_new ){
    setup->n_term_byte = n_new-1;
    setup->i_term_off = setup->t->terms.n;
    FTS5_BUFFER_APPEND_BLOB(&p->rc, &setup->t->terms, n_new-1, p_new+1);
  }

  while( 0==__fts5_poslist_next64(
     p1->base.data, p1->base.n_data, &i_pos_off, &i_pos
  ) ){
    __fts5_tokendata_iter_append_map(p, 
        setup->t, setup->i_term_off, setup->n_term_byte, p1->base.i_rowid, i_pos
    );
  }
}


/*
** Context object passed by __fts5_setup_prefix_iter() to __fts5_visit_entries().
*/
typedef struct prefix_setup_ctx_t prefix_setup_ctx_t;
struct prefix_setup_ctx_t {
  void (*merge)(fts5_index_t*, fts5_buffer_t*, int, fts5_buffer_t*);
  void (*append)(fts5_index_t*, u_int64_t, fts5_iter_t*, fts5_buffer_t*);
  int64_t i_last_rowid;
  int n_merge;
  fts5_buffer_t *bufs;
  int n_buf;
  fts5_buffer_t doclist;
  tokendata_setup_ctx_t *tokendata;
};

/*
** __fts5_visit_entries() callback used by __fts5_setup_prefix_iter()
*/
static void __prefix_iter_setup_cb(
  fts5_index_t *p, 
  void *ctx, 
  fts5_iter_t *p1, 
  const u_int8_t *p_new,
  int n_new
){
  prefix_setup_ctx_t *setup = (prefix_setup_ctx_t*)ctx;
  const int n_merge = setup->n_merge;

  if( p1->base.n_data>0 ){
    if( p1->base.i_rowid<=setup->i_last_rowid && setup->doclist.n>0 ){
      int i;
      for(i=0; p->rc==DBSQL_OK && setup->doclist.n; i++){
        int i1 = i*n_merge;
        int i_store;
        assert( i1+n_merge<=setup->n_buf );
        for(i_store=i1; i_store<i1+n_merge; i_store++){
          if( setup->bufs[i_store].n==0 ){
            __fts5_buffer_swap(&setup->doclist, &setup->bufs[i_store]);
            FTS5_BUFFER_ZERO(&setup->doclist);
            break;
          }
        }
        if( i_store==i1+n_merge ){
          setup->merge(p, &setup->doclist, n_merge, &setup->bufs[i1]);
          for(i_store=i1; i_store<i1+n_merge; i_store++){
            FTS5_BUFFER_ZERO(&setup->bufs[i_store]);
          }
        }
      }
      setup->i_last_rowid = 0;
    }

    setup->append(
        p, (u_int64_t)p1->base.i_rowid-(u_int64_t)setup->i_last_rowid, p1, &setup->doclist
    );
    setup->i_last_rowid = p1->base.i_rowid;
  }

  if( setup->tokendata ){
    __prefix_iter_setup_tokendata_cb(p, (void*)setup->tokendata, p1, p_new, n_new);
  }
}

static void __fts5_setup_prefix_iter(
  fts5_index_t *p,                   /* index_t to read from */
  int desc,                      /* True for "ORDER BY rowid DESC" */
  int i_idx,                       /* index_t to scan for data */
  u_int8_t *token,                     /* Buffer containing prefix to match */
  int n_token,                     /* Size of buffer token in bytes */
  fts5_colset_t *colset,            /* Restrict matches to these columns */
  fts5_iter_t **iter               /* OUT: New iterator */
){
  fts5_structure_t *p_struct;
  prefix_setup_ctx_t s;
  tokendata_setup_ctx_t s2;

  memset(&s, 0, sizeof(s));
  memset(&s2, 0, sizeof(s2));

  s.n_merge = 1;
  s.i_last_rowid = 0;
  s.n_buf = 32;
  if( i_idx==0 
   && p->config->detail==FTS5_DETAIL_FULL 
   && p->config->prefix_insttoken 
  ){
    s.tokendata = &s2;
    s2.t = (fts5_token_data_iter_t*)__fts5_idx_malloc(p, SZ_FTS5TOKENDATAITER(1));
  }

  if( p->config->detail==FTS5_DETAIL_NONE ){
    s.merge = __fts5_merge_rowid_lists;
    s.append = __fts5_append_rowid;
  }else{
    s.n_merge = FTS5_MERGE_NLIST-1;
    s.n_buf = s.n_merge*8;   /* Sufficient to merge (16^8)==(2^32) lists */
    s.merge = __fts5_merge_prefix_lists;
    s.append = __fts5_append_poslist;
  }

  s.bufs = (fts5_buffer_t*)__fts5_idx_malloc(p, sizeof(fts5_buffer_t)*s.n_buf);
  p_struct = __fts5_structure_read(p);
  assert( p->rc!=DBSQL_OK || (s.bufs && p_struct) );

  if( p->rc==DBSQL_OK ){
    void *ctx = (void*)&s;
    int i;
    fts5_data_t *data;

    /* If i_idx is non-zero, then it is the number of a prefix-index for
    ** prefixes 1 character longer than the prefix being queried for. That
    ** index contains all the doclists required, except for the one
    ** corresponding to the prefix itself. That one is extracted from the
    ** main term index here.  */
    if( i_idx!=0 ){
      token[0] = FTS5_MAIN_PREFIX;
      __fts5_visit_entries(p, colset, token, n_token, 0, __prefix_iter_setup_cb, ctx);
    }

    token[0] = FTS5_MAIN_PREFIX + i_idx;
    __fts5_visit_entries(p, colset, token, n_token, 1, __prefix_iter_setup_cb, ctx);

    assert( (s.n_buf%s.n_merge)==0 );
    for(i=0; i<s.n_buf; i+=s.n_merge){
      int i_free;
      if( p->rc==DBSQL_OK ){
        s.merge(p, &s.doclist, s.n_merge, &s.bufs[i]);
      }
      for(i_free=i; i_free<i+s.n_merge; i_free++){
        FTS5_BUFFER_FREE(&s.bufs[i_free]);
      }
    }

    data = __fts5_idx_malloc(p, sizeof(*data)
                             + ((int64_t)s.doclist.n)+FTS5_DATA_ZERO_PADDING);
    assert( data!=0 || p->rc!=DBSQL_OK );
    if( data ){
      data->p = (u_int8_t*)&data[1];
      data->nn = data->sz_leaf = s.doclist.n;
      if( s.doclist.n ) memcpy(data->p, s.doclist.p, s.doclist.n);
      __fts5_multi_iter_new2(p, data, desc, iter);
    }

    assert( (*iter)!=0 || p->rc!=DBSQL_OK );
    if( p->rc==DBSQL_OK && s.tokendata ){
      __fts5_tokendata_iter_sort_map(p, s2.t);
      (*iter)->token_data_iter = s2.t;
      s2.t = 0;
    }
  }

  __fts5_tokendata_iter_delete(s2.t);
  FTS5_BUFFER_FREE(&s.doclist);
  __fts5_structure_release_local(p_struct);
  dbsql_free(s.bufs);
}


/*
** Indicate that all subsequent calls to __fts5_index_write() pertain
** to the document with rowid i_rowid.
*/
int __fts5_index_begin_write(fts5_index_t *p, int b_delete, int64_t i_rowid){
  assert( p->rc==DBSQL_OK );

  /* Allocate the hash table if it has not already been allocated */
  if( p->hash==0 ){
    p->rc = __fts5_hash_new(p->config, &p->hash, &p->n_pending_data);
  }

  /* Flush the hash table to disk if required */
  if( i_rowid<p->i_write_rowid 
   || (i_rowid==p->i_write_rowid && p->b_delete==0)
   || (p->n_pending_data > p->config->n_hash_size)
  ){
    __fts5_index_flush(p);
  }

  p->i_write_rowid = i_rowid;
  p->b_delete = b_delete;
  if( b_delete==0 ){
    p->n_pending_row++;
  }
  return __fts5_index_return(p);
}

/*
** Commit data to disk.
*/
int __fts5_index_sync(fts5_index_t *p){
  assert( p->rc==DBSQL_OK );
  __fts5_index_flush(p);
  __fts5_index_close_reader_local(p);
  return __fts5_index_return(p);
}

/*
** Discard any data stored in the in-memory hash tables. Do not write it
** to the database. Additionally, assume that the contents of the %_data
** table may have changed on disk. So any in-memory caches of %_data 
** records must be invalidated.
*/
int __fts5_index_rollback(fts5_index_t *p){
  __fts5_index_close_reader_local(p);
  __fts5_index_discard_data(p);
  __fts5_structure_invalidate(p);
  return __fts5_index_return(p);
}

/*
** The %_data table is completely empty when this function is called. This
** function populates it with the initial structure objects for each index,
** and the initial version of the "averages" record (a zero-byte blob).
*/
int __fts5_index_reinit(fts5_index_t *p){
  fts5_structure_t *tmp;
  union {
    fts5_structure_t s_fts;
    u_int8_t tmp_space[SZ_FTS5STRUCTURE(1)];
  } u_fts;
  __fts5_structure_invalidate(p);
  __fts5_index_discard_data(p);
  tmp = &u_fts.s_fts;
  memset(u_fts.tmp_space, 0, sizeof(u_fts.tmp_space));
  if( p->config->contentless_delete ){
    tmp->n_origin_cntr = 1;
  }
  __fts5_data_write(p, FTS5_AVERAGES_ROWID, (const u_int8_t*)"", 0);
  __fts5_structure_write(p, tmp);
  return __fts5_index_return(p);
}

/*
** Open a new fts5_index_t handle. If the create argument is true, create
** and initialize the underlying %_data table.
**
** If successful, set *pp to point to the new object and return DBSQL_OK.
** Otherwise, set *pp to NULL and return an dbsql error code.
*/
int __fts5_index_open(
  fts5_config_t *config, 
  int create, 
  fts5_index_t **pp,
  char **err_p
){
  int rc = DBSQL_OK;
  fts5_index_t *p;                   /* New object */

  *pp = p = (fts5_index_t*)__fts5_malloc_zero(&rc, sizeof(fts5_index_t));
  if( rc==DBSQL_OK ){
    p->config = config;
    p->n_work_unit = FTS5_WORK_UNIT;
    p->data_tbl = __fts5_mprintf(&rc, "%s_data", config->name);
    if( p->data_tbl && create ){
      rc = __fts5_create_table(
          config, "data", "id INTEGER PRIMARY KEY, block BLOB", 0, err_p
      );
      if( rc==DBSQL_OK ){
        rc = __fts5_create_table(config, "idx", 
            "segid, term, pgno, PRIMARY KEY(segid, term)", 
            1, err_p
        );
      }
      if( rc==DBSQL_OK ){
        rc = __fts5_index_reinit(p);
      }
    }
  }

  assert( rc!=DBSQL_OK || p->rc==DBSQL_OK );
  if( rc ){
    __fts5_index_close(p);
    *pp = 0;
  }
  return rc;
}

/*
** Close a handle opened by an earlier call to __fts5_index_open().
*/
int __fts5_index_close(fts5_index_t *p){
  int rc = DBSQL_OK;
  if( p ){
    assert( p->reader==0 );
    __fts5_structure_invalidate(p);
    dbsql_finalize(p->writer);
    dbsql_finalize(p->deleter);
    dbsql_finalize(p->idx_writer);
    dbsql_finalize(p->idx_deleter);
    dbsql_finalize(p->idx_select);
    dbsql_finalize(p->idx_next_select);
    dbsql_finalize(p->data_version);
    dbsql_finalize(p->delete_from_idx);
    __fts5_hash_free(p->hash);
    dbsql_free(p->data_tbl);
    dbsql_free(p);
  }
  return rc;
}

/*
** Argument p points to a buffer containing utf-8 text that is n bytes in 
** size. Return the number of bytes in the n_char character prefix of the
** buffer, or 0 if there are less than n_char characters in total.
*/
int __fts5_index_charlen_to_bytelen(
  const char *p, 
  int n_byte, 
  int n_char
){
  int n = 0;
  int i;
  for(i=0; i<n_char; i++){
    if( n>=n_byte ) return 0;      /* Input contains fewer than n_char chars */
    if( (unsigned char)p[n++]>=0xc0 ){
      if( n>=n_byte ) return 0;
      while( (p[n] & 0xc0)==0x80 ){
        n++;
        if( n>=n_byte ){
          if( i+1==n_char ) break;
          return 0;
        }
      }
    }
  }
  return n;
}

/*
** in is a UTF-8 encoded string, n_in bytes in size. Return the number of
** unicode characters in the string.
*/
static int __fts5_index_charlen(const char *in, int n_in){
  int n_char = 0;            
  int i = 0;
  while( i<n_in ){
    if( (unsigned char)in[i++]>=0xc0 ){
      while( i<n_in && (in[i] & 0xc0)==0x80 ) i++;
    }
    n_char++;
  }
  return n_char;
}

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
){
  int i;                          /* Used to iterate through indexes */
  int rc = DBSQL_OK;             /* Return code */
  fts5_config_t *config = p->config;

  assert( p->rc==DBSQL_OK );
  assert( (i_col<0)==p->b_delete );

  /* Add the entry to the main terms index. */
  rc = __fts5_hash_write(
      p->hash, p->i_write_rowid, i_col, i_pos, FTS5_MAIN_PREFIX, token, n_token
  );

  for(i=0; i<config->n_prefix && rc==DBSQL_OK; i++){
    const int n_char = config->__a_prefix[i];
    int n_byte = __fts5_index_charlen_to_bytelen(token, n_token, n_char);
    if( n_byte ){
      rc = __fts5_hash_write(p->hash, 
          p->i_write_rowid, i_col, i_pos, (char)(FTS5_MAIN_PREFIX+i+1), token,
          n_byte
      );
    }
  }

  return rc;
}

/*
** token points to a buffer of size n_token bytes containing a search 
** term, including the index number at the start, used on a tokendata=1
** table. This function returns true if the term in buffer buf matches 
** token token/n_token.
*/
static int __fts5_is_tokendata_prefix(
  fts5_buffer_t *buf,
  const u_int8_t *token,
  int n_token
){
  return (
      buf->n>=n_token 
   && 0==memcmp(buf->p, token, n_token)
   && (buf->n==n_token || buf->p[n_token]==0x00)
  );
}

/*
** Ensure the segment-iterator passed as the only argument points to EOF.
*/
static void __fts5_seg_iter_set_eof(fts5_seg_iter_t *seg){
  __fts5_data_release(seg->leaf);
  seg->leaf = 0;
}

static void __fts5_iter_close_local(fts5_index_iter_t *index_iter){
  if( index_iter ){
    fts5_iter_t *iter = (fts5_iter_t*)index_iter;
    fts5_index_t *p_index = iter->p_index;
    __fts5_tokendata_iter_delete(iter->token_data_iter);
    __fts5_multi_iter_free(iter);
    __fts5_index_close_reader_local(p_index);
  }
}

/*
** This function appends iterator append to fts5_token_data_iter_t in and 
** returns the result.
*/
static fts5_token_data_iter_t *__fts5_append_tokendata_iter(
  fts5_index_t *p,                   /* index_t object (for error code) */
  fts5_token_data_iter_t *in,         /* Current fts5_token_data_iter_t struct */
  fts5_iter_t *append               /* Append this iterator */
){
  fts5_token_data_iter_t *ret = in;

  if( p->rc==DBSQL_OK ){
    if( in==0 || in->n_iter==in->n_iter_alloc ){
      int64_t n_alloc = in ? in->n_iter_alloc*2 : 16;
      int64_t n_byte = SZ_FTS5TOKENDATAITER(n_alloc+1);
      fts5_token_data_iter_t *p_new;
      p_new = (fts5_token_data_iter_t*)dbsql_realloc64(in, n_byte);

      if( p_new==0 ){
        p->rc = DBSQL_NOMEM;
      }else{
        if( in==0 ) memset(p_new, 0, n_byte);
        ret = p_new;
        p_new->n_iter_alloc = n_alloc;
      }
    }
  }
  if( p->rc ){
    __fts5_iter_close_local((fts5_index_iter_t*)append);
  }else{
    ret->iters[ret->n_iter++] = append;
  }
  assert( ret==0 || ret->n_iter<=ret->n_iter_alloc );

  return ret;
}

/*
** The iterator passed as the only argument must be a tokendata=1 iterator
** (iter->token_data_iter!=0). This function sets the iterator output
** variables (iter->base.*) according to the contents of the current
** row.
*/
static void __fts5_iter_set_outputs_tokendata(fts5_iter_t *iter){
  int ii;
  int n_hit = 0;
  int64_t i_rowid = SMALLEST_INT64;
  int i_min = 0;

  fts5_token_data_iter_t *t = iter->token_data_iter;

  iter->base.n_data = 0;
  iter->base.data = 0;

  for(ii=0; ii<t->n_iter; ii++){
    fts5_iter_t *p = t->iters[ii];
    if( p->base.eof==0 ){
      if( n_hit==0 || p->base.i_rowid<i_rowid ){
        i_rowid = p->base.i_rowid;
        n_hit = 1;
        iter->base.data = p->base.data;
        iter->base.n_data = p->base.n_data;
        i_min = ii;
      }else if( p->base.i_rowid==i_rowid ){
        n_hit++;
      }
    }
  }

  if( n_hit==0 ){
    iter->base.eof = 1;
  }else{
    int detail = iter->p_index->config->detail;
    iter->base.eof = 0;
    iter->base.i_rowid = i_rowid;

    if( n_hit==1 && detail==FTS5_DETAIL_FULL ){
      __fts5_tokendata_iter_append_map(iter->p_index, t, i_min, 0, i_rowid, -1);
    }else
    if( n_hit>1 && detail!=FTS5_DETAIL_NONE ){
      int n_reader = 0;
      int n_byte = 0;
      int64_t i_prev = 0;

      /* Allocate array of iterators if they are not already allocated. */
      if( t->poslist_readers==0 ){
        t->poslist_readers = (fts5_poslist_reader_t*)__fts5_malloc_zero(
            &iter->p_index->rc,
            t->n_iter * (sizeof(fts5_poslist_reader_t) + sizeof(int))
        );
        if( t->poslist_readers==0 ) return;
        t->poslist_to_iters = (int*)&t->poslist_readers[t->n_iter];
      }

      /* Populate an iterator for each poslist that will be merged */
      for(ii=0; ii<t->n_iter; ii++){
        fts5_iter_t *p = t->iters[ii];
        if( i_rowid==p->base.i_rowid ){
          t->poslist_to_iters[n_reader] = ii;
          __fts5_poslist_reader_init(
              p->base.data, p->base.n_data, &t->poslist_readers[n_reader++]
          );
          n_byte += p->base.n_data;
        }
      }

      /* Ensure the output buffer is large enough */
      if( FTS5_BUFFER_GROW(&iter->p_index->rc, &iter->poslist, n_byte+n_hit*10) ){
        return;
      }

      /* Ensure the token-mapping is large enough */
      if( detail==FTS5_DETAIL_FULL && t->n_map_alloc<(t->n_map + n_byte) ){
        int64_t n_new = (t->n_map_alloc + n_byte) * 2;
        fts5_token_data_map_t *news = (fts5_token_data_map_t*)dbsql_realloc64(
            t->maps, n_new*sizeof(fts5_token_data_map_t)
        );
        if( news==0 ){
          iter->p_index->rc = DBSQL_NOMEM;
          return;
        }
        t->maps = news;
        t->n_map_alloc = n_new;
      }

      iter->poslist.n = 0;

      while( 1 ){
        int64_t i_min_pos = LARGEST_INT64;

        /* Find smallest position */
        i_min = 0;
        for(ii=0; ii<n_reader; ii++){
          fts5_poslist_reader_t *reader = &t->poslist_readers[ii];
          if( reader->eof==0 ){
            if( reader->i_pos<i_min_pos ){
              i_min_pos = reader->i_pos;
              i_min = ii;
            }
          }
        }

        /* If all readers were at EOF, break out of the loop. */
        if( i_min_pos==LARGEST_INT64 ) break;

        __fts5_poslist_safe_append(&iter->poslist, &i_prev, i_min_pos);
        __fts5_poslist_reader_next(&t->poslist_readers[i_min]);

        if( detail==FTS5_DETAIL_FULL ){
          t->maps[t->n_map].i_pos = i_min_pos;
          t->maps[t->n_map].i_iter = t->poslist_to_iters[i_min];
          t->maps[t->n_map].i_rowid = i_rowid;
          t->n_map++;
        }
      }

      iter->base.data = iter->poslist.p;
      iter->base.n_data = iter->poslist.n;
    }
  }
}

/*
** The iterator passed as the only argument must be a tokendata=1 iterator
** (iter->token_data_iter!=0). This function advances the iterator. If
** argument from is false, then the iterator is advanced to the next
** entry. Or, if from is true, it is advanced to the first entry with
** a rowid of i_from or greater.
*/
static void __fts5_tokendata_iter_next(fts5_iter_t *iter, int from, int64_t i_from){
  int ii;
  fts5_token_data_iter_t *t = iter->token_data_iter;
  fts5_index_t *p_index = iter->p_index;

  for(ii=0; ii<t->n_iter; ii++){
    fts5_iter_t *p = t->iters[ii];
    if( p->base.eof==0 
     && (p->base.i_rowid==iter->base.i_rowid || (from && p->base.i_rowid<i_from))
    ){
      __fts5_multi_iter_next(p_index, p, from, i_from);
      while( from && p->base.eof==0 
          && p->base.i_rowid<i_from 
          && p_index->rc==DBSQL_OK 
      ){
        __fts5_multi_iter_next(p_index, p, 0, 0);
      }
    }
  }

  if( p_index->rc==DBSQL_OK ){
    __fts5_iter_set_outputs_tokendata(iter);
  }
}

/*
** If the segment-iterator passed as the first argument is at EOF, then
** set iter->term to a copy of buffer term.
*/
static void __fts5_tokendata_set_term_if_eof(fts5_iter_t *iter, fts5_buffer_t *term){
  if( iter && iter->segs[0].leaf==0 ){
    FTS5_BUFFER_SET(&iter->p_index->rc, &iter->segs[0].term, term->n, term->p);
  }
}

/*
** This function sets up an iterator to use for a non-prefix query on a 
** tokendata=1 table. 
*/
static fts5_iter_t *__fts5_setup_tokendata_iter(
  fts5_index_t *p,                   /* FTS index to query */
  const u_int8_t *token,               /* Buffer containing query term */
  int n_token,                     /* Size of buffer token in bytes */
  fts5_colset_t *colset             /* Colset to filter on */
){
  fts5_iter_t *ret = 0;
  fts5_token_data_iter_t *set = 0;
  fts5_structure_t *p_struct = 0;
  const int flags = FTS5INDEX_QUERY_SCANONETERM | FTS5INDEX_QUERY_SCAN;

  fts5_buffer_t seek = {0, 0, 0};
  fts5_buffer_t *small = 0;             

  __fts5_index_flush(p);
  p_struct = __fts5_structure_read(p);

  while( p->rc==DBSQL_OK ){
    fts5_iter_t *prev = set ? set->iters[set->n_iter-1] : 0;
    fts5_iter_t *p_new = 0;
    fts5_seg_iter_t *new_iter = 0;
    fts5_seg_iter_t *prev_iter = 0;

    int i_lvl, i_seg, ii;

    p_new = __fts5_multi_iter_alloc(p, p_struct->n_segment);
    if( small ){
      FTS5_BUFFER_SET(&p->rc, &seek, small->n, small->p);
      FTS5_BUFFER_APPEND_BLOB(&p->rc, &seek, 1, (const u_int8_t*)"\0");
    }else{
      FTS5_BUFFER_SET(&p->rc, &seek, n_token, token);
    }
    if( p->rc ){
      __fts5_iter_close_local((fts5_index_iter_t*)p_new);
      break;
    }

    new_iter = &p_new->segs[0];
    prev_iter = (prev ? &prev->segs[0] : 0);
    for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
      for(i_seg=p_struct->levels[i_lvl].n_seg-1; i_seg>=0; i_seg--){
        fts5_structure_segment_t *seg = &p_struct->levels[i_lvl].segs[i_seg];
        int done = 0;

        if( prev_iter ){
          if( __fts5_buffer_compare(small, &prev_iter->term) ){
            memcpy(new_iter, prev_iter, sizeof(fts5_seg_iter_t));
            memset(prev_iter, 0, sizeof(fts5_seg_iter_t));
            done = 1;
          }else if( prev_iter->i_endof_doclist>prev_iter->leaf->sz_leaf ){
            __fts5_seg_iter_next_init(p,(const char*)seek.p,seek.n-1,seg,new_iter);
            done = 1;
          }
        }

        if( done==0 ){
          __fts5_seg_iter_seek_init(p, seek.p, seek.n, flags, seg, new_iter);
        }

        if( prev_iter ){
          if( prev_iter->tomb_array ){
            new_iter->tomb_array = prev_iter->tomb_array;
            new_iter->tomb_array->n_ref++;
          }
        }else{
          __fts5_seg_iter_alloc_tombstone(p, new_iter);
        }

        new_iter++;
        if( prev_iter ) prev_iter++;
        if( p->rc ) break;
      }
    }
    __fts5_tokendata_set_term_if_eof(prev, small);

    p_new->skip_empty = 1;
    p_new->colset = colset;
    __fts5_iter_set_output_cb(&p->rc, p_new);

    /* Loop through all segments in the new iterator. Find the smallest 
    ** term that any segment-iterator points to. Iterator p_new will be
    ** used for this term. Also, set any iterator that points to a term that
    ** does not match token/n_token to point to EOF */
    small = 0;
    for(ii=0; ii<p_new->n_seg; ii++){
      fts5_seg_iter_t *p_ii = &p_new->segs[ii];
      if( 0==__fts5_is_tokendata_prefix(&p_ii->term, token, n_token) ){
        __fts5_seg_iter_set_eof(p_ii);
      }
      if( p_ii->leaf && (!small || __fts5_buffer_compare(small, &p_ii->term)>0) ){
        small = &p_ii->term;
      }
    }

    /* If small is still NULL at this point, then the new iterator does
    ** not point to any terms that match the query. So delete it and break
    ** out of the loop - all required iterators have been collected.  */
    if( small==0 ){
      __fts5_iter_close_local((fts5_index_iter_t*)p_new);
      break;
    }

    /* Append this iterator to the set and continue. */
    set = __fts5_append_tokendata_iter(p, set, p_new);
  }

  if( p->rc==DBSQL_OK && set ){
    int ii;
    for(ii=0; ii<set->n_iter; ii++){
      fts5_iter_t *iter = set->iters[ii];
      int i_seg;
      for(i_seg=0; i_seg<iter->n_seg; i_seg++){
        iter->segs[i_seg].flags |= FTS5_SEGITER_ONETERM;
      }
      __fts5_multi_iter_finish_setup(p, iter);
    }
  }
    
  if( p->rc==DBSQL_OK ){
    ret = __fts5_multi_iter_alloc(p, 0);
  }
  if( ret ){
    ret->n_seg = 0;
    ret->token_data_iter = set;
    if( set ){
      __fts5_iter_set_outputs_tokendata(ret);
    }else{
      ret->base.eof = 1;
    }
  }else{
    __fts5_tokendata_iter_delete(set);
  }

  __fts5_structure_release_local(p_struct);
  FTS5_BUFFER_FREE(&seek);
  return ret;
}

/*
** Open a new iterator to iterate though all rowid that match the 
** specified token or token prefix.
*/
int __fts5_index_query(
  fts5_index_t *p,                   /* FTS index to query */
  const char *token, int n_token, /* Token (or prefix) to query for */
  int flags,                      /* Mask of FTS5INDEX_QUERY_X flags */
  fts5_colset_t *colset,            /* Match these columns only */
  fts5_index_iter_t **iter          /* OUT: New iterator object */
){
  fts5_config_t *config = p->config;
  fts5_iter_t *ret = 0;
  fts5_buffer_t buf = {0, 0, 0};

  /* If the QUERY_SCAN flag is set, all other flags must be clear. */
  assert( (flags & FTS5INDEX_QUERY_SCAN)==0 || flags==FTS5INDEX_QUERY_SCAN );

  if( __fts5_buffer_size(&p->rc, &buf, n_token+1)==0 ){
    int i_idx = 0;                 /* index_t to search */
    int i_prefix_idx = 0;           /* +1 prefix index */
    int tokendata = config->tokendata;
    assert( buf.p!=0 );
    if( n_token>0 ) memcpy(&buf.p[1], token, n_token);

    /* The NOTOKENDATA flag is set when each token in a tokendata=1 table
    ** should be treated individually, instead of merging all those with
    ** a common prefix into a single entry. This is used, for example, by
    ** queries performed as part of an integrity-check, or by the fts5vocab
    ** module.  */
    if( flags & (FTS5INDEX_QUERY_NOTOKENDATA|FTS5INDEX_QUERY_SCAN) ){
      tokendata = 0;
    }

    /* Figure out which index to search and set i_idx accordingly. If this
    ** is a prefix query for which there is no prefix index, set i_idx to
    ** greater than config->n_prefix to indicate that the query will be
    ** satisfied by scanning multiple terms in the main index.
    **
    ** If the QUERY_TEST_NOIDX flag was specified, then this must be a
    ** prefix-query. Instead of using a prefix-index (if one exists), 
    ** evaluate the prefix query using the main FTS index. This is used
    ** for internal sanity checking by the integrity-check in debug 
    ** mode only.  */
#ifdef DBSQL_DEBUG
    if( config->prefix_index==0 || (flags & FTS5INDEX_QUERY_TEST_NOIDX) ){
      assert( flags & FTS5INDEX_QUERY_PREFIX );
      i_idx = 1+config->n_prefix;
    }else
#endif
    if( flags & FTS5INDEX_QUERY_PREFIX ){
      int n_char = __fts5_index_charlen(token, n_token);
      for(i_idx=1; i_idx<=config->n_prefix; i_idx++){
        int n_idx_char = config->__a_prefix[i_idx-1];
        if( n_idx_char==n_char ) break;
        if( n_idx_char==n_char+1 ) i_prefix_idx = i_idx;
      }
    }

    if( tokendata && i_idx==0 ){
      buf.p[0] = FTS5_MAIN_PREFIX;
      ret = __fts5_setup_tokendata_iter(p, buf.p, n_token+1, colset);
    }else if( i_idx<=config->n_prefix ){
      /* Straight index lookup */
      fts5_structure_t *p_struct = __fts5_structure_read(p);
      buf.p[0] = (u_int8_t)(FTS5_MAIN_PREFIX + i_idx);
      if( p_struct ){
        __fts5_multi_iter_new(p, p_struct, flags | FTS5INDEX_QUERY_SKIPEMPTY, 
            colset, buf.p, n_token+1, -1, 0, &ret
        );
        __fts5_structure_release_local(p_struct);
      }
    }else{
      /* Scan multiple terms in the main index for a prefix query. */
      int desc = (flags & FTS5INDEX_QUERY_DESC)!=0;
      __fts5_setup_prefix_iter(p, desc, i_prefix_idx, buf.p, n_token+1, colset,&ret);
      if( ret==0 ){
        assert( p->rc!=DBSQL_OK );
      }else{
        assert( ret->colset==0 );
        __fts5_iter_set_output_cb(&p->rc, ret);
        if( p->rc==DBSQL_OK ){
          fts5_seg_iter_t *seg = &ret->segs[ret->firsts[1].i_first];
          if( seg->leaf ) ret->set_outputs(ret, seg);
        }
      }
    }

    if( p->rc ){
      __fts5_iter_close_local((fts5_index_iter_t*)ret);
      ret = 0;
      __fts5_index_close_reader_local(p);
    }

    *iter = (fts5_index_iter_t*)ret;
    __fts5_buffer_free(&buf);
  }
  return __fts5_index_return(p);
}

/*
** Return true if the iterator passed as the only argument is at EOF.
*/
/*
** Move to the next matching rowid. 
*/
int __fts5_iter_next(fts5_index_iter_t *index_iter){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  assert( iter->p_index->rc==DBSQL_OK );
  if( iter->n_seg==0 ){
    assert( iter->token_data_iter );
    __fts5_tokendata_iter_next(iter, 0, 0);
  }else{
    __fts5_multi_iter_next(iter->p_index, iter, 0, 0);
  }
  return __fts5_index_return(iter->p_index);
}

/*
** Move to the next matching term/rowid. Used by the fts5vocab module.
*/
int __fts5_iter_next_scan(fts5_index_iter_t *index_iter){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  fts5_index_t *p = iter->p_index;

  assert( iter->p_index->rc==DBSQL_OK );

  __fts5_multi_iter_next(p, iter, 0, 0);
  if( p->rc==DBSQL_OK ){
    fts5_seg_iter_t *seg = &iter->segs[ iter->firsts[1].i_first ];
    if( seg->leaf && seg->term.p[0]!=FTS5_MAIN_PREFIX ){
      __fts5_data_release(seg->leaf);
      seg->leaf = 0;
      iter->base.eof = 1;
    }
  }

  return __fts5_index_return(iter->p_index);
}

/*
** Move to the next matching rowid that occurs at or after i_match. The
** definition of "at or after" depends on whether this iterator iterates
** in ascending or descending rowid order.
*/
int __fts5_iter_next_from(fts5_index_iter_t *index_iter, int64_t i_match){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  if( iter->n_seg==0 ){
    assert( iter->token_data_iter );
    __fts5_tokendata_iter_next(iter, 1, i_match);
  }else{
    __fts5_multi_iter_next_from(iter->p_index, iter, i_match);
  }
  return __fts5_index_return(iter->p_index);
}

/*
** Return the current term.
*/
const char *__fts5_iter_term(fts5_index_iter_t *index_iter, int *pn){
  int n;
  const char *z = (const char*)__fts5_multi_iter_term((fts5_iter_t*)index_iter, &n);
  assert_nc( z || n<=1 );
  *pn = n-1;
  return (z ? &z[1] : 0);
}

/*
** iter is a prefix query. This function populates iter->token_data_iter
** with an fts5_token_data_iter_t object containing mappings for all rows
** matched by the query.
*/
static int __fts5_setup_prefix_iter_tokendata(
  fts5_iter_t *iter,
  const char *p_token,             /* Token prefix to search for */
  int n_token                      /* Size of token in bytes */
){
  fts5_index_t *p = iter->p_index;
  fts5_buffer_t token = {0, 0, 0};
  tokendata_setup_ctx_t ctx;

  memset(&ctx, 0, sizeof(ctx));

  FTS5_BUFFER_GROW(&p->rc, &token, n_token+1);
  assert( token.p!=0 || p->rc!=DBSQL_OK );
  ctx.t = (fts5_token_data_iter_t*)__fts5_malloc_zero(&p->rc,
                                                   SZ_FTS5TOKENDATAITER(1));

  if( p->rc==DBSQL_OK ){

    /* Fill in the token prefix to search for */
    token.p[0] = FTS5_MAIN_PREFIX;
    memcpy(&token.p[1], p_token, n_token);
    token.n = n_token+1;

    __fts5_visit_entries(
        p, 0, token.p, token.n, 1, __prefix_iter_setup_tokendata_cb, (void*)&ctx
    );

    __fts5_tokendata_iter_sort_map(p, ctx.t);
  }

  if( p->rc==DBSQL_OK ){
    iter->token_data_iter = ctx.t;
  }else{
    __fts5_tokendata_iter_delete(ctx.t);
  }
  FTS5_BUFFER_FREE(&token);

  return __fts5_index_return(p);
}

/*
** This is used by inst_token() to access the token at offset i_off, column
** i_col of row i_rowid. The token is returned via output variables *out
** and *pn_out. The iterator passed as the first argument must be a tokendata=1
** iterator (iter->token_data_iter!=0).
**
** token/n_token:
*/
int __fts5_iter_token(
  fts5_index_iter_t *index_iter, 
  const char *token, int n_token,
  int64_t i_rowid,
  int i_col, 
  int i_off, 
  const char **out, int *pn_out
){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  fts5_token_data_iter_t *t = iter->token_data_iter;
  int64_t i_pos = (((int64_t)i_col)<<32) + i_off;
  fts5_token_data_map_t *maps = 0;
  int i1 = 0;
  int i2 = 0;
  int i_test = 0;

  assert( t || (token && iter->n_seg>0) );
  if( t==0 ){
    int rc = __fts5_setup_prefix_iter_tokendata(iter, token, n_token);
    if( rc!=DBSQL_OK ) return rc;
    t = iter->token_data_iter;
  }

  i2 = t->n_map;
  maps = t->maps;

  while( i2>i1 ){
    i_test = (i1 + i2) / 2;

    if( maps[i_test].i_rowid<i_rowid ){
      i1 = i_test+1;
    }else if( maps[i_test].i_rowid>i_rowid ){
      i2 = i_test;
    }else{
      if( maps[i_test].i_pos<i_pos ){
        if( maps[i_test].i_pos<0 ){
          break;
        }
        i1 = i_test+1;
      }else if( maps[i_test].i_pos>i_pos ){
        i2 = i_test;
      }else{
        break;
      }
    }
  }

  if( i2>i1 ){
    if( iter->n_seg==0 ){
      fts5_iter_t *map = t->iters[maps[i_test].i_iter];
      *out = (const char*)map->segs[0].term.p+1;
      *pn_out = map->segs[0].term.n-1;
    }else{
      fts5_token_data_map_t *p = &maps[i_test];
      *out = (const char*)&t->terms.p[p->i_iter];
      *pn_out = maps[i_test].n_byte;
    }
  }

  return DBSQL_OK;
}

/*
** Clear any existing entries from the token-map associated with the
** iterator passed as the only argument. 
*/
void __fts5_index_iter_clear_tokendata(fts5_index_iter_t *index_iter){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  if( iter && iter->token_data_iter 
   && (iter->n_seg==0 || iter->p_index->config->detail!=FTS5_DETAIL_FULL)
  ){
    iter->token_data_iter->n_map = 0;
  }
}

/*
** Set a token-mapping for the iterator passed as the first argument. This
** is used in detail=column or detail=none mode when a token is requested
** using the inst_token() API. In this case the caller tokenizers the
** current row and configures the token-mapping via multiple calls to this
** function.
*/
int __fts5_index_iter_write_tokendata(
  fts5_index_iter_t *index_iter, 
  const char *token, int n_token, 
  int64_t i_rowid, int i_col, int i_off
){
  fts5_iter_t *iter = (fts5_iter_t*)index_iter;
  fts5_token_data_iter_t *t = iter->token_data_iter;
  fts5_index_t *p = iter->p_index;
  int64_t i_pos = (((int64_t)i_col)<<32) + i_off;

  assert( p->config->detail!=FTS5_DETAIL_FULL );
  assert( iter->token_data_iter || iter->n_seg>0 );
  if( iter->n_seg>0 ){
    /* This is a prefix term iterator. */
    if( t==0 ){
      t = (fts5_token_data_iter_t*)__fts5_malloc_zero(&p->rc,
                                           SZ_FTS5TOKENDATAITER(1));
      iter->token_data_iter = t;
    }
    if( t ){
      __fts5_tokendata_iter_append_map(p, t, t->terms.n, n_token, i_rowid, i_pos);
      FTS5_BUFFER_APPEND_BLOB(&p->rc, &t->terms, n_token, (const u_int8_t*)token);
    }
  }else{
    int ii;
    for(ii=0; ii<t->n_iter; ii++){
      fts5_buffer_t *term = &t->iters[ii]->segs[0].term;
      if( n_token==term->n-1 && memcmp(token, term->p+1, n_token)==0 ) break;
    }
    if( ii<t->n_iter ){
      __fts5_tokendata_iter_append_map(p, t, ii, 0, i_rowid, i_pos);
    }
  }
  return __fts5_index_return(p);
}

/*
** Close an iterator opened by an earlier call to __fts5_index_query().
*/
void __fts5_iter_close(fts5_index_iter_t *index_iter){
  if( index_iter ){
    fts5_index_t *p_index = ((fts5_iter_t*)index_iter)->p_index;
    __fts5_iter_close_local(index_iter);
    __fts5_index_return(p_index);
  }
}

/*
** Read and decode the "averages" record from the database. 
**
** Parameter an_size must point to an array of size n_col, where n_col is
** the number of user defined columns in the FTS table.
*/
int __fts5_index_get_averages(fts5_index_t *p, int64_t *pn_row, int64_t *an_size){
  int n_col = p->config->n_col;
  fts5_data_t *data;

  *pn_row = 0;
  memset(an_size, 0, sizeof(int64_t) * n_col);
  data = __fts5_data_read(p, FTS5_AVERAGES_ROWID);
  if( p->rc==DBSQL_OK && data->nn ){
    int i = 0;
    int i_col;
    i += __fts5_get_varint(&data->p[i], (u_int64_t*)pn_row);
    for(i_col=0; i<data->nn && i_col<n_col; i_col++){
      i += __fts5_get_varint(&data->p[i], (u_int64_t*)&an_size[i_col]);
    }
  }

  __fts5_data_release(data);
  return __fts5_index_return(p);
}

/*
** Replace the current "averages" record with the contents of the buffer 
** supplied as the second argument.
*/
int __fts5_index_set_averages(fts5_index_t *p, const u_int8_t *data, int n_data){
  assert( p->rc==DBSQL_OK );
  __fts5_data_write(p, FTS5_AVERAGES_ROWID, data, n_data);
  return __fts5_index_return(p);
}

/*
** Return the total number of blocks this module has read from the %_data
** table since it was created.
*/
int __fts5_index_reads(fts5_index_t *p){
  return p->n_read;
}

/*
** Set the 32-bit cookie value stored at the start of all structure 
** records to the value passed as the second argument.
**
** Return DBSQL_OK if successful, or an dbsql error code if an error
** occurs.
*/
int __fts5_index_set_cookie(fts5_index_t *p, int i_new){
  int rc;                              /* Return code */
  fts5_config_t *config = p->config;    /* Configuration object */
  u_int8_t cookies[4];                       /* Binary representation of i_new */
  dbsql_blob_t *blob = 0;

  assert( p->rc==DBSQL_OK );
  __fts5_put32(cookies, i_new);

  rc = dbsql_blob_open(config->db, config->z_db, p->data_tbl, 
      "block", FTS5_STRUCTURE_ROWID, 1, &blob
  );
  if( rc==DBSQL_OK ){
    dbsql_blob_write(blob, cookies, 4, 0);
    rc = dbsql_blob_close(blob);
  }

  return rc;
}

int __fts5_index_load_config(fts5_index_t *p){
  fts5_structure_t *p_struct;
  p_struct = __fts5_structure_read(p);
  __fts5_structure_release_local(p_struct);
  return __fts5_index_return(p);
}

/*
** Retrieve the origin value that will be used for the segment currently
** being accumulated in the in-memory hash table when it is flushed to
** disk. If successful, DBSQL_OK is returned and (*pi_origin) set to
** the queried value. Or, if an error occurs, an error code is returned
** and the final value of (*pi_origin) is undefined.
*/
int __fts5_index_get_origin(fts5_index_t *p, int64_t *pi_origin){
  fts5_structure_t *p_struct;
  p_struct = __fts5_structure_read(p);
  if( p_struct ){
    *pi_origin = p_struct->n_origin_cntr;
    __fts5_structure_release_local(p_struct);
  }
  return __fts5_index_return(p);
}

/*
** Buffer pg contains a page of a tombstone hash table - one of n_pg pages
** associated with the same segment. This function adds rowid i_rowid to
** the hash table. The caller is required to guarantee that there is at
** least one free slot on the page.
**
** If parameter force is false and the hash table is deemed to be full
** (more than half of the slots are occupied), then non-zero is returned
** and i_rowid not inserted. Or, if force is true or if the hash table page
** is not full, i_rowid is inserted and zero returned.
*/
static int __fts5_index_tombstone_add_to_page(
  fts5_data_t *pg, 
  int force,
  int n_pg, 
  u_int64_t i_rowid
){
  const int sz_key = TOMBSTONE_KEYSIZE(pg);
  const int n_slot = TOMBSTONE_NSLOT(pg);
  const int n_elem = __fts5_get_u32(&pg->p[4]);
  int i_slot = (i_rowid / n_pg) % n_slot;
  int n_collide = n_slot;

  if( sz_key==4 && i_rowid>0xFFFFFFFF ) return 2;
  if( i_rowid==0 ){
    pg->p[1] = 0x01;
    return 0;
  }

  if( force==0 && n_elem>=(n_slot/2) ){
    return 1;
  }

  __fts5_put_u32(&pg->p[4], n_elem+1);
  if( sz_key==4 ){
    u_int32_t *slots = (u_int32_t*)&pg->p[8];
    while( slots[i_slot] ){
      i_slot = (i_slot + 1) % n_slot;
      if( n_collide--==0 ) return 0;
    }
    __fts5_put_u32((u_int8_t*)&slots[i_slot], (u_int32_t)i_rowid);
  }else{
    u_int64_t *slots = (u_int64_t*)&pg->p[8];
    while( slots[i_slot] ){
      i_slot = (i_slot + 1) % n_slot;
      if( n_collide--==0 ) return 0;
    }
    __fts5_put_u64((u_int8_t*)&slots[i_slot], i_rowid);
  }

  return 0;
}

/*
** This function attempts to build a new hash containing all the keys 
** currently in the tombstone hash table for segment seg. The new
** hash will be stored in the n_out buffers passed in array outs[].
** All pages of the new hash use key-size sz_key (4 or 8).
**
** Return 0 if the hash is successfully rebuilt into the n_out pages. 
** Or non-zero if it is not (because one page became overfull). In this 
** case the caller should retry with a larger n_out parameter.
**
** Parameter data1 is page i_pg1 of the hash table being rebuilt.
*/
static int __fts5_index_tombstone_rehash(
  fts5_index_t *p,
  fts5_structure_segment_t *seg,     /* Segment to rebuild hash of */
  fts5_data_t *data1,               /* One page of current hash - or NULL */
  int i_pg1,                       /* Which page of the current hash is data1 */
  int sz_key,                      /* 4 or 8, the keysize */
  int n_out,                       /* Number of output pages */
  fts5_data_t **outs                /* Array of output hash pages */
){
  int ii;
  int res = 0;

  /* Initialize the headers of all the output pages */
  for(ii=0; ii<n_out; ii++){
    outs[ii]->p[0] = sz_key;
    __fts5_put_u32(&outs[ii]->p[4], 0);
  }

  /* Loop through the current pages of the hash table. */ 
  for(ii=0; res==0 && ii<seg->n_pg_tombstone; ii++){
    fts5_data_t *data = 0;          /* Page ii of the current hash table */
    fts5_data_t *p_free = 0;          /* Free this at the end of the loop */

    if( i_pg1==ii ){
      data = data1;
    }else{
      p_free = data = __fts5_data_read(p, FTS5_TOMBSTONE_ROWID(seg->i_segid, ii));
    }

    if( data ){
      int sz_key_in = TOMBSTONE_KEYSIZE(data);
      int n_slot_in = (data->nn - 8) / sz_key_in;
      int i_in;
      for(i_in=0; i_in<n_slot_in; i_in++){
        u_int64_t i_val = 0;

        /* Read the value from slot i_in of the input page into i_val. */
        if( sz_key_in==4 ){
          u_int32_t *slots = (u_int32_t*)&data->p[8];
          if( slots[i_in] ) i_val = __fts5_get_u32((u_int8_t*)&slots[i_in]);
        }else{
          u_int64_t *slots = (u_int64_t*)&data->p[8];
          if( slots[i_in] ) i_val = __fts5_get_u64((u_int8_t*)&slots[i_in]);
        }

        /* If i_val is not 0 at this point, insert it into the new hash table */
        if( i_val ){
          fts5_data_t *pg = outs[(i_val % n_out)];
          res = __fts5_index_tombstone_add_to_page(pg, 0, n_out, i_val);
          if( res ) break;
        }
      }

      /* If this is page 0 of the old hash, copy the rowid-0-flag from the
      ** old hash to the new.  */
      if( ii==0 ){
        outs[0]->p[1] = data->p[1];
      }
    }
    __fts5_data_release(p_free);
  }

  return res;
}

/*
** This is called to rebuild the hash table belonging to segment seg.
** If parameter data1 is not NULL, then one page of the existing hash table
** has already been loaded - data1, which is page i_pg1. The key-size for
** the new hash table is sz_key (4 or 8).
**
** If successful, the new hash table is not written to disk. Instead, 
** output parameter (*pn_out) is set to the number of pages in the new
** hash table, and (*pap_out) to point to an array of buffers containing
** the new page data.
**
** If an error occurs, an error code is left in the fts5_index_t object and
** both output parameters set to 0 before returning.
*/
static void __fts5_index_tombstone_rebuild(
  fts5_index_t *p,
  fts5_structure_segment_t *seg,     /* Segment to rebuild hash of */
  fts5_data_t *data1,               /* One page of current hash - or NULL */
  int i_pg1,                       /* Which page of the current hash is data1 */
  int sz_key,                      /* 4 or 8, the keysize */
  int *pn_out,                     /* OUT: Number of output pages */
  fts5_data_t ***pap_out              /* OUT: Output hash pages */
){
  const int MINSLOT = 32;
  int n_slot_per_page = MAX(MINSLOT, (p->config->pgsz - 8) / sz_key);
  int64_t n_slot = 0;                  /* Number of slots in each output page */
  int64_t n_out = 0;

  /* Figure out how many output pages (n_out) and how many slots per 
  ** page (n_slot).  There are three possibilities: 
  **
  **   1. The hash table does not yet exist. In this case the new hash
  **      table will consist of a single page with MINSLOT slots.
  **
  **   2. The hash table exists but is currently a single page. In this
  **      case an attempt is made to grow the page to accommodate the new
  **      entry. The page is allowed to grow up to n_slot_per_page (see above)
  **      slots.
  **
  **   3. The hash table already consists of more than one page, or of
  **      a single page already so large that it cannot be grown. In this
  **      case the new hash consists of (n_pg*2+1) pages of n_slot_per_page
  **      slots each, where n_pg is the current number of pages in the 
  **      hash table.
  */
  if( seg->n_pg_tombstone==0 ){
    /* Case 1. */
    n_out = 1;
    n_slot = MINSLOT;
  }else if( seg->n_pg_tombstone==1 ){
    /* Case 2. */
    u_int32_t n_elem = __fts5_get_u32(&data1->p[4]);
    assert( data1 && i_pg1==0 );
    if( n_elem>((u_int32_t)n_slot_per_page/4) ){
      n_out = 0;
    }else{
      n_out = 1;
      n_slot = MAX((int64_t)n_elem*4, MINSLOT);
    }
  }
  if( n_out==0 ){
    /* Case 3. */
    n_out = ((int64_t)seg->n_pg_tombstone * 2 + 1);
    n_slot = n_slot_per_page;
  }

  /* Allocate the required array and output pages */
  while( 1 ){
    int res = 0;
    int64_t ii = 0;
    int64_t sz_page = 0;
    fts5_data_t **outs = 0;

    /* Allocate space for the new hash table */
    assert( n_slot>=MINSLOT );
    outs = (fts5_data_t**)__fts5_malloc_zero(&p->rc, sizeof(fts5_data_t*) * n_out);
    sz_page = 8 + n_slot*sz_key;
    for(ii=0; ii<n_out; ii++){
      fts5_data_t *p_new = (fts5_data_t*)__fts5_malloc_zero(&p->rc, 
          sizeof(fts5_data_t)+sz_page
      );
      if( p_new ){
        p_new->nn = sz_page;
        p_new->p = (u_int8_t*)&p_new[1];
        outs[ii] = p_new;
      }
    }

    /* Rebuild the hash table. */
    if( p->rc==DBSQL_OK ){
      res = __fts5_index_tombstone_rehash(p, seg, data1, i_pg1, sz_key, n_out, outs);
    }
    if( res==0 ){
      if( p->rc ){
        __fts5_index_free_array(outs, n_out);
        outs = 0;
        n_out = 0;
      }
      *pn_out = n_out;
      *pap_out = outs;
      break;
    }
    
    /* If control flows to here, it was not possible to rebuild the hash
    ** table. Free all buffers and then try again with more pages. */
    assert( p->rc==DBSQL_OK );
    __fts5_index_free_array(outs, n_out);
    n_slot = n_slot_per_page;
    n_out = n_out*2 + 1;
  }
}


/*
** Add a tombstone for rowid i_rowid to segment seg.
*/
static void __fts5_index_tombstone_add(
  fts5_index_t *p, 
  fts5_structure_segment_t *seg, 
  u_int64_t i_rowid
){
  fts5_data_t *pg = 0;
  int i_pg = -1;
  int sz_key = 0;
  int n_hash = 0;
  fts5_data_t **hashes = 0;

  p->n_contentless_delete++;

  if( seg->n_pg_tombstone>0 ){
    i_pg = i_rowid % seg->n_pg_tombstone;
    pg = __fts5_data_read(p, FTS5_TOMBSTONE_ROWID(seg->i_segid,i_pg));
    if( pg==0 ){
      assert( p->rc!=DBSQL_OK );
      return;
    }

    if( 0==__fts5_index_tombstone_add_to_page(pg, 0, seg->n_pg_tombstone, i_rowid) ){
      __fts5_data_write(p, FTS5_TOMBSTONE_ROWID(seg->i_segid,i_pg), pg->p, pg->nn);
      __fts5_data_release(pg);
      return;
    }
  }

  /* Have to rebuild the hash table. First figure out the key-size (4 or 8). */
  sz_key = pg ? TOMBSTONE_KEYSIZE(pg) : 4;
  if( i_rowid>0xFFFFFFFF ) sz_key = 8;

  /* Rebuild the hash table */
  __fts5_index_tombstone_rebuild(p, seg, pg, i_pg, sz_key, &n_hash, &hashes);
  assert( p->rc==DBSQL_OK || (n_hash==0 && hashes==0) );

  /* If all has succeeded, write the new rowid into one of the new hash
  ** table pages, then write them all out to disk. */
  if( n_hash ){
    int ii = 0;
    __fts5_index_tombstone_add_to_page(hashes[i_rowid % n_hash], 1, n_hash, i_rowid);
    for(ii=0; ii<n_hash; ii++){
      int64_t i_tombstone_rowid = FTS5_TOMBSTONE_ROWID(seg->i_segid, ii);
      __fts5_data_write(p, i_tombstone_rowid, hashes[ii]->p, hashes[ii]->nn);
    }
    seg->n_pg_tombstone = n_hash;
    __fts5_structure_write(p, p->p_struct);
  }

  __fts5_data_release(pg);
  __fts5_index_free_array(hashes, n_hash);
}

/*
** Add i_rowid to the tombstone list of the segment or segments that contain
** rows from origin i_origin. Return DBSQL_OK if successful, or an dbsql
** error code otherwise.
*/
int __fts5_index_contentless_delete(fts5_index_t *p, int64_t i_origin, int64_t i_rowid){
  fts5_structure_t *p_struct;
  p_struct = __fts5_structure_read(p);
  if( p_struct ){
    int found = 0;               /* True after seg->n_entry_tombstone incr. */
    int i_lvl;
    for(i_lvl=p_struct->n_level-1; i_lvl>=0; i_lvl--){
      int i_seg;
      for(i_seg=p_struct->levels[i_lvl].n_seg-1; i_seg>=0; i_seg--){
        fts5_structure_segment_t *seg = &p_struct->levels[i_lvl].segs[i_seg];
        if( seg->i_origin1<=(u_int64_t)i_origin && seg->i_origin2>=(u_int64_t)i_origin ){
          if( found==0 ){
            seg->n_entry_tombstone++;
            found = 1;
          }
          __fts5_index_tombstone_add(p, seg, i_rowid);
        }
      }
    }
    __fts5_structure_release_local(p_struct);
  }
  return __fts5_index_return(p);
}

/*************************************************************************
**************************************************************************
** Below this point is the implementation of the integrity-check 
** functionality.
*/

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
){
  int i;
  u_int64_t ret = i_rowid;
  ret += (ret<<3) + i_col;
  ret += (ret<<3) + i_pos;
  if( i_idx>=0 ) ret += (ret<<3) + (FTS5_MAIN_PREFIX + i_idx);
  for(i=0; i<n_term; i++) ret += (ret<<3) + term[i];
  return ret;
}

#ifdef DBSQL_DEBUG
/*
** This function is purely an internal test. It does not contribute to 
** FTS functionality, or even the integrity-check, in any way.
**
** Instead, it tests that the same set of pgno/rowid combinations are 
** visited regardless of whether the doclist-index identified by parameters
** i_segid/i_leaf is iterated in forwards or reverse order.
*/
static void FTS5_TEST_DLIDX_REVERSE(
  fts5_index_t *p, 
  int i_segid,                     /* Segment id to load from */
  int i_leaf                       /* Load doclist-index for this leaf */
){
  fts5_dlidx_iter_t *dlidx = 0;
  u_int64_t cksum1 = 13;
  u_int64_t cksum2 = 13;

  for(dlidx=__fts5_dlidx_iter_init(p, 0, i_segid, i_leaf);
      __fts5_dlidx_iter_eof(p, dlidx)==0;
      __fts5_dlidx_iter_next(p, dlidx)
  ){
    int64_t i_rowid = __fts5_dlidx_iter_rowid(dlidx);
    int pgno = __fts5_dlidx_iter_pgno(dlidx);
    assert( pgno>i_leaf );
    cksum1 += i_rowid + ((int64_t)pgno<<32);
  }
  __fts5_dlidx_iter_free(dlidx);
  dlidx = 0;

  for(dlidx=__fts5_dlidx_iter_init(p, 1, i_segid, i_leaf);
      __fts5_dlidx_iter_eof(p, dlidx)==0;
      __fts5_dlidx_iter_prev(p, dlidx)
  ){
    int64_t i_rowid = __fts5_dlidx_iter_rowid(dlidx);
    int pgno = __fts5_dlidx_iter_pgno(dlidx);
    assert( __fts5_dlidx_iter_pgno(dlidx)>i_leaf );
    cksum2 += i_rowid + ((int64_t)pgno<<32);
  }
  __fts5_dlidx_iter_free(dlidx);
  dlidx = 0;

  if( p->rc==DBSQL_OK && cksum1!=cksum2 ) p->rc = FTS5_CORRUPT;
}

static int __fts5_query_cksum(
  fts5_index_t *p,                   /* Fts5 index object */
  int i_idx,
  const char *z,                  /* index_t key to query for */
  int n,                          /* Size of index key in bytes */
  int flags,                      /* Flags for __fts5_index_query */
  u_int64_t *p_cksum                     /* IN/OUT: Checksum value */
){
  int detail = p->config->detail;
  u_int64_t cksum = *p_cksum;
  fts5_index_iter_t *iter = 0;
  int rc = __fts5_index_query(
      p, z, n, (flags | FTS5INDEX_QUERY_NOTOKENDATA), 0, &iter
  );

  while( rc==DBSQL_OK && ALWAYS(iter!=0) && 0==__fts5_iter_eof(iter) ){
    int64_t rowid = iter->i_rowid;

    if( detail==FTS5_DETAIL_NONE ){
      cksum ^= __fts5_index_entry_cksum(rowid, 0, 0, i_idx, z, n);
    }else{
      fts5_poslist_reader_t s_reader;
      for(__fts5_poslist_reader_init(iter->data, iter->n_data, &s_reader);
          s_reader.eof==0;
          __fts5_poslist_reader_next(&s_reader)
      ){
        int i_col = FTS5_POS2COLUMN(s_reader.i_pos);
        int i_off = FTS5_POS2OFFSET(s_reader.i_pos);
        cksum ^= __fts5_index_entry_cksum(rowid, i_col, i_off, i_idx, z, n);
      }
    }
    if( rc==DBSQL_OK ){
      rc = __fts5_iter_next(iter);
    }
  }
  __fts5_iter_close_local(iter);

  *p_cksum = cksum;
  return rc;
}

/*
** Check if buffer z[], size n bytes, contains as series of valid utf-8
** encoded codepoints. If so, return 0. Otherwise, if the buffer does not
** contain valid utf-8, return non-zero.
*/
static int __fts5_test_utf8(const char *z, int n){
  int i = 0;
  assert_nc( n>0 );
  while( i<n ){
    if( (z[i] & 0x80)==0x00 ){
      i++;
    }else
    if( (z[i] & 0xE0)==0xC0 ){
      if( i+1>=n || (z[i+1] & 0xC0)!=0x80 ) return 1;
      i += 2;
    }else
    if( (z[i] & 0xF0)==0xE0 ){
      if( i+2>=n || (z[i+1] & 0xC0)!=0x80 || (z[i+2] & 0xC0)!=0x80 ) return 1;
      i += 3;
    }else
    if( (z[i] & 0xF8)==0xF0 ){
      if( i+3>=n || (z[i+1] & 0xC0)!=0x80 || (z[i+2] & 0xC0)!=0x80 ) return 1;
      if( (z[i+2] & 0xC0)!=0x80 ) return 1;
      i += 3;
    }else{
      return 1;
    }
  }

  return 0;
}

/*
** This function is also purely an internal test. It does not contribute to 
** FTS functionality, or even the integrity-check, in any way.
**
** This function sets output variable (*pb_fail) to true if the test fails. Or
** leaves it unchanged if the test succeeds.
*/
static void FTS5_TEST_TERM(
  fts5_index_t *p, 
  fts5_buffer_t *prev,              /* Previous term */
  const char *z, int n,           /* Possibly new term to test */
  u_int64_t expected,
  u_int64_t *cksum,
  int *pb_fail
){
  int rc = p->rc;
  if( prev->n==0 ){
    FTS5_BUFFER_SET(&rc, prev, n, (const u_int8_t*)z);
  }else
  if( *pb_fail==0 
   && rc==DBSQL_OK 
   && (prev->n!=n || memcmp(prev->p, z, n)) 
   && (p->hash==0 || p->hash->n_entry==0)
  ){
    u_int64_t cksum3 = *cksum;
    const char *term = (const char*)&prev->p[1];  /* term sans prefix-byte */
    int n_term = prev->n-1;            /* Size of term in bytes */
    int i_idx = (prev->p[0] - FTS5_MAIN_PREFIX);
    int flags = (i_idx==0 ? 0 : FTS5INDEX_QUERY_PREFIX);
    u_int64_t ck1 = 0;
    u_int64_t ck2 = 0;

    /* Check that the results returned for ASC and DESC queries are
    ** the same. If not, call this corruption.  */
    rc = __fts5_query_cksum(p, i_idx, term, n_term, flags, &ck1);
    if( rc==DBSQL_OK ){
      int f = flags|FTS5INDEX_QUERY_DESC;
      rc = __fts5_query_cksum(p, i_idx, term, n_term, f, &ck2);
    }
    if( rc==DBSQL_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;

    /* If this is a prefix query, check that the results returned if the
    ** the index is disabled are the same. In both ASC and DESC order. 
    **
    ** This check may only be performed if the hash table is empty. This
    ** is because the hash table only supports a single scan query at
    ** a time, and the multi-iter loop from which this function is called
    ** is already performing such a scan. 
    **
    ** Also only do this if buffer term contains n_term bytes of valid
    ** utf-8. Otherwise, the last part of the buffer contents might contain
    ** a non-utf-8 sequence that happens to be a prefix of a valid utf-8
    ** character stored in the main fts index, which will cause the
    ** test to fail.  */
    if( p->n_pending_data==0 && 0==__fts5_test_utf8(term, n_term) ){
      if( i_idx>0 && rc==DBSQL_OK ){
        int f = flags|FTS5INDEX_QUERY_TEST_NOIDX;
        ck2 = 0;
        rc = __fts5_query_cksum(p, i_idx, term, n_term, f, &ck2);
        if( rc==DBSQL_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;
      }
      if( i_idx>0 && rc==DBSQL_OK ){
        int f = flags|FTS5INDEX_QUERY_TEST_NOIDX|FTS5INDEX_QUERY_DESC;
        ck2 = 0;
        rc = __fts5_query_cksum(p, i_idx, term, n_term, f, &ck2);
        if( rc==DBSQL_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;
      }
    }

    cksum3 ^= ck1;
    FTS5_BUFFER_SET(&rc, prev, n, (const u_int8_t*)z);

    if( rc==DBSQL_OK && cksum3!=expected ){
      *pb_fail = 1;
    }
    *cksum = cksum3;
  }
  p->rc = rc;
}
 
#else
# define FTS5_TEST_DLIDX_REVERSE(x,y,z)
# define FTS5_TEST_TERM(t,u,v,w,x,y,z)
#endif

/*
** Check that:
**
**   1) All leaves of seg between i_first and i_last (inclusive) exist and
**      contain zero terms.
**   2) All leaves of seg between i_no_rowid and i_last (inclusive) exist and
**      contain zero rowids.
*/
static void __fts5_index_integrity_check_empty(
  fts5_index_t *p,
  fts5_structure_segment_t *seg,     /* Segment to check internal consistency */
  int i_first,
  int i_no_rowid,
  int i_last
){
  int i;

  /* Now check that the iter.n_empty leaves following the current leaf
  ** (a) exist and (b) contain no terms. */
  for(i=i_first; p->rc==DBSQL_OK && i<=i_last; i++){
    fts5_data_t *leaf = __fts5_leaf_read(p, FTS5_SEGMENT_ROWID(seg->i_segid, i));
    if( leaf ){
      if( !FTS5_LEAF_IS_TERMLESS(leaf)
       || (i>=i_no_rowid && 0!=FTS5_LEAF_FIRST_ROWID_OFF(leaf))
      ){
        FTS5_CORRUPT_ROWID(p, FTS5_SEGMENT_ROWID(seg->i_segid, i));
      }
    }
    __fts5_data_release(leaf);
  }
}

static void __fts5_integrity_check_pgidx(fts5_index_t *p, int64_t i_rowid, fts5_data_t *leaf){
  int64_t i_term_off = 0;
  int ii;

  fts5_buffer_t buf1 = {0,0,0};
  fts5_buffer_t buf2 = {0,0,0};

  ii = leaf->sz_leaf;
  while( ii<leaf->nn && p->rc==DBSQL_OK ){
    int res;
    int64_t i_off;
    int n_incr;

    ii += FTS5_GET_VARINT32(&leaf->p[ii], n_incr);
    i_term_off += n_incr;
    i_off = i_term_off;

    if( i_off>=leaf->sz_leaf ){
      FTS5_CORRUPT_ROWID(p, i_rowid);
    }else if( i_term_off==n_incr ){
      int n_byte;
      i_off += FTS5_GET_VARINT32(&leaf->p[i_off], n_byte);
      if( (i_off+n_byte)>leaf->sz_leaf ){
        FTS5_CORRUPT_ROWID(p, i_rowid);
      }else{
        FTS5_BUFFER_SET(&p->rc, &buf1, n_byte, &leaf->p[i_off]);
      }
    }else{
      int n_keep, n_byte;
      i_off += FTS5_GET_VARINT32(&leaf->p[i_off], n_keep);
      i_off += FTS5_GET_VARINT32(&leaf->p[i_off], n_byte);
      if( n_keep>buf1.n || (i_off+n_byte)>leaf->sz_leaf ){
        FTS5_CORRUPT_ROWID(p, i_rowid);
      }else{
        buf1.n = n_keep;
        FTS5_BUFFER_APPEND_BLOB(&p->rc, &buf1, n_byte, &leaf->p[i_off]);
      }

      if( p->rc==DBSQL_OK ){
        res = __fts5_buffer_compare(&buf1, &buf2);
        if( res<=0 ) FTS5_CORRUPT_ROWID(p, i_rowid);
      }
    }
    FTS5_BUFFER_SET(&p->rc, &buf2, buf1.n, buf1.p);
  }

  FTS5_BUFFER_FREE(&buf1);
  FTS5_BUFFER_FREE(&buf2);
}

static void __fts5_index_integrity_check_segment(
  fts5_index_t *p,                   /* FTS5 backend object */
  fts5_structure_segment_t *seg      /* Segment to check internal consistency */
){
  fts5_config_t *config = p->config;
  int secure_delete = (config->i_version==FTS5_CURRENT_VERSION_SECUREDELETE);
  dbsql_stmt_t *stmt = 0;
  int rc2;
  int i_idx_prev_leaf = seg->pgno_first-1;
  int i_dlidx_prev_leaf = seg->pgno_last;

  if( seg->pgno_first==0 ) return;

  __fts5_index_prepare_stmt(p, &stmt, dbsql_mprintf(
      "SELECT segid, term, (pgno>>1), (pgno&1) FROM %Q.'%q_idx' WHERE segid=%d "
      "ORDER BY 1, 2",
      config->z_db, config->name, seg->i_segid
  ));

  /* Iterate through the b-tree hierarchy.  */
  while( p->rc==DBSQL_OK && DBSQL_ROW==dbsql_step(stmt) ){
    int64_t i_row;                     /* Rowid for this leaf */
    fts5_data_t *leaf;              /* Data for this leaf */

    const char *idx_term = (const char*)dbsql_column_blob(stmt, 1);
    int n_idx_term = dbsql_column_bytes(stmt, 1);
    int i_idx_leaf = dbsql_column_int(stmt, 2);
    int idx_dlidx = dbsql_column_int(stmt, 3);

    /* If the leaf in question has already been trimmed from the segment, 
    ** ignore this b-tree entry. Otherwise, load it into memory. */
    if( i_idx_leaf<seg->pgno_first ) continue;
    i_row = FTS5_SEGMENT_ROWID(seg->i_segid, i_idx_leaf);
    leaf = __fts5_leaf_read(p, i_row);
    if( leaf==0 ) break;

    /* Check that the leaf contains at least one term, and that it is equal
    ** to or larger than the split-key in idx_term.  Also check that if there
    ** is also a rowid pointer within the leaf page header, it points to a
    ** location before the term.  */
    if( leaf->nn<=leaf->sz_leaf ){

      if( n_idx_term==0 
       && config->i_version==FTS5_CURRENT_VERSION_SECUREDELETE
       && leaf->nn==leaf->sz_leaf
       && leaf->nn==4
      ){
        /* special case - the very first page in a segment keeps its %_idx
        ** entry even if all the terms are removed from it by secure-delete 
        ** operations. */
      }else{
        FTS5_CORRUPT_ROWID(p, i_row);
      }

    }else{
      int i_off;                   /* Offset of first term on leaf */
      int i_rowid_off;              /* Offset of first rowid on leaf */
      int n_term;                  /* Size of term on leaf in bytes */
      int res;                    /* Comparison of term and split-key */

      i_off = __fts5_leaf_first_term_off(leaf);
      i_rowid_off = FTS5_LEAF_FIRST_ROWID_OFF(leaf);
      if( i_rowid_off>=i_off || i_off>=leaf->sz_leaf ){
        FTS5_CORRUPT_ROWID(p, i_row);
      }else{
        i_off += FTS5_GET_VARINT32(&leaf->p[i_off], n_term);
        if( (int64_t)i_off+(int64_t)n_term>(int64_t)leaf->sz_leaf ){
          FTS5_CORRUPT_ROWID(p, i_row);
        }else{
          res = FTS5_MEMCMP(&leaf->p[i_off], idx_term, MIN(n_term, n_idx_term));
          if( res==0 ) res = n_term - n_idx_term;
          if( res<0 ) FTS5_CORRUPT_ROWID(p, i_row);
        }
      }

      __fts5_integrity_check_pgidx(p, i_row, leaf);
    }
    __fts5_data_release(leaf);
    if( p->rc ) break;

    /* Now check that the iter.n_empty leaves following the current leaf
    ** (a) exist and (b) contain no terms. */
    __fts5_index_integrity_check_empty(
        p, seg, i_idx_prev_leaf+1, i_dlidx_prev_leaf+1, i_idx_leaf-1
    );
    if( p->rc ) break;

    /* If there is a doclist-index, check that it looks right. */
    if( idx_dlidx ){
      fts5_dlidx_iter_t *dlidx = 0;  /* For iterating through doclist index */
      int i_prev_leaf = i_idx_leaf;
      int i_segid = seg->i_segid;
      int i_pg = 0;
      int64_t i_key;

      for(dlidx=__fts5_dlidx_iter_init(p, 0, i_segid, i_idx_leaf);
          __fts5_dlidx_iter_eof(p, dlidx)==0;
          __fts5_dlidx_iter_next(p, dlidx)
      ){

        /* Check any rowid-less pages that occur before the current leaf. */
        for(i_pg=i_prev_leaf+1; i_pg<__fts5_dlidx_iter_pgno(dlidx); i_pg++){
          i_key = FTS5_SEGMENT_ROWID(i_segid, i_pg);
          leaf = __fts5_leaf_read(p, i_key);
          if( leaf ){
            if( FTS5_LEAF_FIRST_ROWID_OFF(leaf)!=0 ) FTS5_CORRUPT_ROWID(p, i_key);
            __fts5_data_release(leaf);
          }
        }
        i_prev_leaf = __fts5_dlidx_iter_pgno(dlidx);

        /* Check that the leaf page indicated by the iterator really does
        ** contain the rowid suggested by the same. */
        i_key = FTS5_SEGMENT_ROWID(i_segid, i_prev_leaf);
        leaf = __fts5_leaf_read(p, i_key);
        if( leaf ){
          int64_t i_rowid;
          int i_rowid_off = FTS5_LEAF_FIRST_ROWID_OFF(leaf);
          ASSERT_SZLEAF_OK(leaf);
          if( i_rowid_off>=leaf->sz_leaf ){
            FTS5_CORRUPT_ROWID(p, i_key);
          }else if( secure_delete==0 || i_rowid_off>0 ){
            int64_t i_dl_rowid = __fts5_dlidx_iter_rowid(dlidx);
            __fts5_get_varint(&leaf->p[i_rowid_off], (u_int64_t*)&i_rowid);
            if( i_rowid<i_dl_rowid || (secure_delete==0 && i_rowid!=i_dl_rowid) ){
              FTS5_CORRUPT_ROWID(p, i_key);
            }
          }
          __fts5_data_release(leaf);
        }
      }

      i_dlidx_prev_leaf = i_pg;
      __fts5_dlidx_iter_free(dlidx);
      FTS5_TEST_DLIDX_REVERSE(p, i_segid, i_idx_leaf);
    }else{
      i_dlidx_prev_leaf = seg->pgno_last;
      /* TODO: Check there is no doclist index */
    }

    i_idx_prev_leaf = i_idx_leaf;
  }

  rc2 = dbsql_finalize(stmt);
  if( p->rc==DBSQL_OK ) p->rc = rc2;

  /* Page iter.i_leaf must now be the rightmost leaf-page in the segment */
#if 0
  if( p->rc==DBSQL_OK && iter.i_leaf!=seg->pgno_last ){
    p->rc = FTS5_CORRUPT;
  }
#endif
}


/*
** Run internal checks to ensure that the FTS index (a) is internally 
** consistent and (b) contains entries for which the XOR of the checksums
** as calculated by __fts5_index_entry_cksum() is cksum.
**
** Return DBSQL_CORRUPT if any of the internal checks fail, or if the
** checksum does not match. Return DBSQL_OK if all checks pass without
** error, or some other dbsql error code if another error (e.g. OOM)
** occurs.
*/
int __fts5_index_integrity_check(fts5_index_t *p, u_int64_t cksum, int use_cksum){
  int detail = p->config->detail;
  u_int64_t cksum2 = 0;                 /* Checksum based on contents of indexes */
  fts5_buffer_t poslist = {0,0,0};   /* Buffer used to hold a poslist */
  fts5_iter_t *iter;                /* Used to iterate through entire index */
  fts5_structure_t *p_struct;         /* index_t structure */
  int i_lvl, i_seg;

#ifdef DBSQL_DEBUG
  /* Used by extra internal tests only run if NDEBUG is not defined */
  u_int64_t cksum3 = 0;                 /* Checksum based on contents of indexes */
  fts5_buffer_t term = {0,0,0};      /* Buffer used to hold most recent term */
  int test_fail = 0;
#endif
  const int flags = FTS5INDEX_QUERY_NOOUTPUT;
  
  /* Load the FTS index structure */
  p_struct = __fts5_structure_read(p);
  if( p_struct==0 ){
    assert( p->rc!=DBSQL_OK );
    return __fts5_index_return(p);
  }

  /* Check that the internal nodes of each segment match the leaves */
  for(i_lvl=0; i_lvl<p_struct->n_level; i_lvl++){
    for(i_seg=0; i_seg<p_struct->levels[i_lvl].n_seg; i_seg++){
      fts5_structure_segment_t *seg = &p_struct->levels[i_lvl].segs[i_seg];
      __fts5_index_integrity_check_segment(p, seg);
    }
  }

  /* The cksum argument passed to this function is a checksum calculated
  ** based on all expected entries in the FTS index (including prefix index
  ** entries). This block checks that a checksum calculated based on the
  ** actual contents of FTS index is identical.
  **
  ** Two versions of the same checksum are calculated. The first (stack
  ** variable cksum2) based on entries extracted from the full-text index
  ** while doing a linear scan of each individual index in turn. 
  **
  ** As each term visited by the linear scans, a separate query for the
  ** same term is performed. cksum3 is calculated based on the entries
  ** extracted by these queries.
  */
  for(__fts5_multi_iter_new(p, p_struct, flags, 0, 0, 0, -1, 0, &iter);
      __fts5_multi_iter_eof(p, iter)==0;
      __fts5_multi_iter_next(p, iter, 0, 0)
  ){
    int n;                      /* Size of term in bytes */
    int64_t i_pos = 0;               /* Position read from poslist */
    int i_off = 0;               /* Offset within poslist */
    int64_t i_rowid = __fts5_multi_iter_rowid(iter);
    char *z = (char*)__fts5_multi_iter_term(iter, &n);

    /* If this is a new term, query for it. Update cksum3 with the results. */
    FTS5_TEST_TERM(p, &term, z, n, cksum2, &cksum3, &test_fail);
    if( p->rc ) break;

    if( detail==FTS5_DETAIL_NONE ){
      if( 0==__fts5_multi_iter_is_empty(p, iter) ){
        cksum2 ^= __fts5_index_entry_cksum(i_rowid, 0, 0, -1, z, n);
      }
    }else{
      poslist.n = 0;
      __fts5_segiter_poslist(p, &iter->segs[iter->firsts[1].i_first], 0, &poslist);
      FTS5_BUFFER_APPEND_BLOB(&p->rc, &poslist, 4, (const u_int8_t*)"\0\0\0\0");
      while( 0==__fts5_poslist_next64(poslist.p, poslist.n, &i_off, &i_pos) ){
        int i_col = FTS5_POS2COLUMN(i_pos);
        int i_tok_off = FTS5_POS2OFFSET(i_pos);
        cksum2 ^= __fts5_index_entry_cksum(i_rowid, i_col, i_tok_off, -1, z, n);
      }
    }
  }
  FTS5_TEST_TERM(p, &term, 0, 0, cksum2, &cksum3, &test_fail);

  __fts5_multi_iter_free(iter);
  if( p->rc==DBSQL_OK && use_cksum && cksum!=cksum2 ){
    p->rc = FTS5_CORRUPT;
    __fts5_config_errmsg(p->config, 
        "fts5: checksum mismatch for table \"%s\"", p->config->name
    );
  }
#ifdef DBSQL_DEBUG
  /* In DBSQL_DEBUG builds, expensive extra checks were run as part of
  ** the integrity-check above. If no other errors were detected, but one
  ** of these tests failed, set the result to DBSQL_CORRUPT_VTAB here. */
  if( p->rc==DBSQL_OK && test_fail ){
    p->rc = FTS5_CORRUPT;
  }
  FTS5_BUFFER_FREE(&term);
#endif

  __fts5_structure_release_local(p_struct);
  FTS5_BUFFER_FREE(&poslist);
  return __fts5_index_return(p);
}

/*************************************************************************
**************************************************************************
** Below this point is the implementation of the fts5_decode() scalar
** function only.
*/

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** Decode a segment-data rowid from the %_data table. This function is
** the opposite of macro FTS5_SEGMENT_ROWID().
*/
static void __fts5_decode_rowid(
  int64_t i_rowid,                     /* Rowid from %_data table */
  int *pb_tombstone,               /* OUT: Tombstone hash flag */
  int *pi_segid,                   /* OUT: Segment id */
  int *pb_dlidx,                   /* OUT: Dlidx flag */
  int *pi_height,                  /* OUT: Height */
  int *pi_pgno                     /* OUT: Page number */
){
  *pi_pgno = (int)(i_rowid & (((int64_t)1 << FTS5_DATA_PAGE_B) - 1));
  i_rowid >>= FTS5_DATA_PAGE_B;

  *pi_height = (int)(i_rowid & (((int64_t)1 << FTS5_DATA_HEIGHT_B) - 1));
  i_rowid >>= FTS5_DATA_HEIGHT_B;

  *pb_dlidx = (int)(i_rowid & 0x0001);
  i_rowid >>= FTS5_DATA_DLI_B;

  *pi_segid = (int)(i_rowid & (((int64_t)1 << FTS5_DATA_ID_B) - 1));
  i_rowid >>= FTS5_DATA_ID_B;

  *pb_tombstone = (int)(i_rowid & 0x0001);
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
static void __fts5_debug_rowid(int *rc, fts5_buffer_t *buf, int64_t i_key){
  int i_segid, i_height, i_pgno, dlidx, tomb;     /* Rowid components */
  __fts5_decode_rowid(i_key, &tomb, &i_segid, &dlidx, &i_height, &i_pgno);

  if( i_segid==0 ){
    if( i_key==FTS5_AVERAGES_ROWID ){
      __fts5_buffer_append_printf(rc, buf, "{averages} ");
    }else{
      __fts5_buffer_append_printf(rc, buf, "{structure}");
    }
  }
  else{
    __fts5_buffer_append_printf(rc, buf, "{%s%ssegid=%d h=%d pgno=%d}",
        dlidx ? "dlidx " : "", 
        tomb ? "tombstone " : "", 
        i_segid, i_height, i_pgno
    );
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
static void __fts5_debug_structure(
  int *rc,                       /* IN/OUT: error code */
  fts5_buffer_t *buf,
  fts5_structure_t *p
){
  int i_lvl, i_seg;                 /* Iterate through levels, segments */

  for(i_lvl=0; i_lvl<p->n_level; i_lvl++){
    fts5_structure_level_t *lvl = &p->levels[i_lvl];
    __fts5_buffer_append_printf(rc, buf, 
        " {lvl=%d nMerge=%d nSeg=%d", i_lvl, lvl->n_merge, lvl->n_seg
    );
    for(i_seg=0; i_seg<lvl->n_seg; i_seg++){
      fts5_structure_segment_t *seg = &lvl->segs[i_seg];
      __fts5_buffer_append_printf(rc, buf, " {id=%d leaves=%d..%d",
          seg->i_segid, seg->pgno_first, seg->pgno_last
      );
      if( seg->i_origin1>0 ){
        __fts5_buffer_append_printf(rc, buf, " origin=%lld..%lld",
            seg->i_origin1, seg->i_origin2
        );
      }
      __fts5_buffer_append_printf(rc, buf, "}");
    }
    __fts5_buffer_append_printf(rc, buf, "}");
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** This is part of the fts5_decode() debugging aid.
**
** Arguments blob/n_blob contain a serialized fts5_structure_t object. This
** function appends a human-readable representation of the same object
** to the buffer passed as the second argument. 
*/
static void __fts5_decode_structure(
  int *p_rc,                       /* IN/OUT: error code */
  fts5_buffer_t *buf,
  const u_int8_t *blob, int n_blob
){
  int rc;                         /* Return code */
  fts5_structure_t *p = 0;           /* Decoded structure object */

  rc = __fts5_structure_decode(blob, n_blob, 0, &p);
  if( rc!=DBSQL_OK ){
    *p_rc = rc;
    return;
  }

  __fts5_debug_structure(p_rc, buf, p);
  __fts5_structure_release_local(p);
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** This is part of the fts5_decode() debugging aid.
**
** Arguments blob/n_blob contain an "averages" record. This function 
** appends a human-readable representation of record to the buffer passed 
** as the second argument. 
*/
static void __fts5_decode_averages(
  int *rc,                       /* IN/OUT: error code */
  fts5_buffer_t *buf,
  const u_int8_t *blob, int n_blob
){
  int i = 0;
  const char *space = "";

  while( i<n_blob ){
    u_int64_t i_val;
    i += __fts5_get_varint(&blob[i], &i_val);
    __fts5_buffer_append_printf(rc, buf, "%s%d", space, (int)i_val);
    space = " ";
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** Buffer (a/n) is assumed to contain a list of serialized varints. Read
** each varint and append its string representation to buffer buf. Return
** after either the input buffer is exhausted or a 0 value is read.
**
** The return value is the number of bytes read from the input buffer.
*/
static int __fts5_decode_poslist(int *rc, fts5_buffer_t *buf, const u_int8_t *a, int n){
  int i_off = 0;
  while( i_off<n ){
    int i_val;
    i_off += FTS5_GET_VARINT32(&a[i_off], i_val);
    __fts5_buffer_append_printf(rc, buf, " %d", i_val);
  }
  return i_off;
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** The start of buffer (a/n) contains the start of a doclist. The doclist
** may or may not finish within the buffer. This function appends a text
** representation of the part of the doclist that is present to buffer
** buf. 
**
** The return value is the number of bytes read from the input buffer.
*/
static int __fts5_decode_doclist(int *rc, fts5_buffer_t *buf, const u_int8_t *a, int n){
  int64_t i_docid = 0;
  int i_off = 0;

  if( n>0 ){
    i_off = __fts5_get_varint(a, (u_int64_t*)&i_docid);
    __fts5_buffer_append_printf(rc, buf, " id=%lld", i_docid);
  }
  while( i_off<n ){
    int n_pos;
    int del;
    i_off += __fts5_get_poslist_size(&a[i_off], &n_pos, &del);
    __fts5_buffer_append_printf(rc, buf, " nPos=%d%s", n_pos, del?"*":"");
    i_off += __fts5_decode_poslist(rc, buf, &a[i_off], MIN(n-i_off, n_pos));
    if( i_off<n ){
      int64_t i_delta;
      i_off += __fts5_get_varint(&a[i_off], (u_int64_t*)&i_delta);
      i_docid += i_delta;
      __fts5_buffer_append_printf(rc, buf, " id=%lld", i_docid);
    }
  }

  return i_off;
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** This function is part of the fts5_decode() debugging function. It is 
** only ever used with detail=none tables.
**
** Buffer (data/n_data) contains a doclist in the format used by detail=none
** tables. This function appends a human-readable version of that list to
** buffer buf.
**
** If *rc is other than DBSQL_OK when this function is called, it is a
** no-op. If an OOM or other error occurs within this function, *rc is
** set to an dbsql error code before returning. The final state of buffer
** buf is undefined in this case.
*/
static void __fts5_decode_rowid_list(
  int *rc,                       /* IN/OUT: Error code */
  fts5_buffer_t *buf,               /* Buffer to append text to */
  const u_int8_t *data, int n_data      /* Data to decode list-of-rowids from */
){
  int i = 0;
  int64_t i_rowid = 0;

  while( i<n_data ){
    const char *app = "";
    u_int64_t i_val;
    i += __fts5_get_varint(&data[i], &i_val);
    i_rowid += i_val;

    if( i<n_data && data[i]==0x00 ){
      i++;
      if( i<n_data && data[i]==0x00 ){
        i++;
        app = "+";
      }else{
        app = "*";
      }
    }

    __fts5_buffer_append_printf(rc, buf, " %lld%s", i_rowid, app);
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
static void __fts5_buffer_append_term(int *rc, fts5_buffer_t *buf, fts5_buffer_t *term){
  int ii;
  FTS5_BUFFER_GROW(rc, buf, term->n*2 + 1);
  if( *rc==DBSQL_OK ){
    for(ii=0; ii<term->n; ii++){
      if( term->p[ii]==0x00 ){
        buf->p[buf->n++] = '\\';
        buf->p[buf->n++] = '0';
      }else{
        buf->p[buf->n++] = term->p[ii];
      }
    }
    buf->p[buf->n] = 0x00;
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
/*
** The implementation of user-defined scalar function fts5_decode().
*/
static void __fts5_decode_function(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args (always 2) */
  dbsql_value_t **vals           /* Function arguments */
){
  int64_t i_rowid;                     /* Rowid for record being decoded */
  int i_segid,i_height,i_pgno,b_dlidx;/* Rowid components */
  int tomb;
  const u_int8_t *blobs; int n;         /* Record to decode */
  u_int8_t *a = 0;
  fts5_buffer_t s;                   /* Build up text to return here */
  int rc = DBSQL_OK;             /* Return code */
  int64_t n_space = 0;
  int detail_none = (dbsql_user_data(ctx)!=0);

  assert( n_arg==2 );
  UNUSED_PARAM(n_arg);
  memset(&s, 0, sizeof(fts5_buffer_t));
  i_rowid = dbsql_value_int64(vals[0]);

  /* Make a copy of the second argument (a blob) in blobs[]. The blobs[]
  ** copy is followed by FTS5_DATA_ZERO_PADDING 0x00 bytes, which prevents
  ** buffer overreads even if the record is corrupt.  */
  n = dbsql_value_bytes(vals[1]);
  blobs = dbsql_value_blob(vals[1]);
  n_space = ((int64_t)n) + FTS5_DATA_ZERO_PADDING;
  a = (u_int8_t*)__fts5_malloc_zero(&rc, n_space);
  if( a==0 ) goto decode_out;
  if( n>0 ) memcpy(a, blobs, n);

  __fts5_decode_rowid(i_rowid, &tomb, &i_segid, &b_dlidx, &i_height, &i_pgno);

  __fts5_debug_rowid(&rc, &s, i_rowid);
  if( b_dlidx ){
    fts5_data_t dlidx;
    fts5_dlidx_lvl_t lvl;

    dlidx.p = a;
    dlidx.nn = n;

    memset(&lvl, 0, sizeof(fts5_dlidx_lvl_t));
    lvl.data = &dlidx;
    lvl.i_leaf_pgno = i_pgno;

    for(__fts5_dlidx_lvl_next(&lvl); lvl.eof==0; __fts5_dlidx_lvl_next(&lvl)){
      __fts5_buffer_append_printf(&rc, &s, 
          " %d(%lld)", lvl.i_leaf_pgno, lvl.i_rowid
      );
    }
  }else if( tomb ){
    u_int32_t n_elem  = __fts5_get_u32(&a[4]);
    int sz_key = (blobs[0]==4 || blobs[0]==8) ? blobs[0] : 8;
    int n_slot = (n - 8) / sz_key;
    int ii;
    __fts5_buffer_append_printf(&rc, &s, " nElem=%d", (int)n_elem);
    if( blobs[1] ){
      __fts5_buffer_append_printf(&rc, &s, " 0");
    }
    for(ii=0; ii<n_slot; ii++){
      u_int64_t i_val = 0;
      if( sz_key==4 ){
        u_int32_t *slots = (u_int32_t*)&blobs[8];
        if( slots[ii] ) i_val = __fts5_get_u32((u_int8_t*)&slots[ii]);
      }else{
        u_int64_t *slots = (u_int64_t*)&blobs[8];
        if( slots[ii] ) i_val = __fts5_get_u64((u_int8_t*)&slots[ii]);
      }
      if( i_val!=0 ){
        __fts5_buffer_append_printf(&rc, &s, " %lld", (int64_t)i_val);
      }
    }
  }else if( i_segid==0 ){
    if( i_rowid==FTS5_AVERAGES_ROWID ){
      __fts5_decode_averages(&rc, &s, a, n);
    }else{
      __fts5_decode_structure(&rc, &s, a, n);
    }
  }else if( detail_none ){
    fts5_buffer_t term;              /* Current term read from page */
    int sz_leaf;
    int i_pgidx_off = sz_leaf = __fts5_get_u16(&a[2]);
    int i_term_off;
    int n_keep = 0;
    int i_off;

    memset(&term, 0, sizeof(fts5_buffer_t));

    /* Decode any entries that occur before the first term. */
    if( sz_leaf<n ){
      i_pgidx_off += FTS5_GET_VARINT32(&a[i_pgidx_off], i_term_off);
    }else{
      i_term_off = sz_leaf;
    }
    __fts5_decode_rowid_list(&rc, &s, &a[4], i_term_off-4);

    i_off = i_term_off;
    while( i_off<sz_leaf && rc==DBSQL_OK ){
      int n_append;

      /* Read the term data for the next term*/
      i_off += FTS5_GET_VARINT32(&a[i_off], n_append);
      term.n = n_keep;
      FTS5_BUFFER_APPEND_BLOB(&rc, &term, n_append, &a[i_off]);
      __fts5_buffer_append_printf(&rc, &s, " term=");
      __fts5_buffer_append_term(&rc, &s, &term);
      i_off += n_append;

      /* Figure out where the doclist for this term ends */
      if( i_pgidx_off<n ){
        int n_incr;
        i_pgidx_off += FTS5_GET_VARINT32(&a[i_pgidx_off], n_incr);
        i_term_off += n_incr;
      }else{
        i_term_off = sz_leaf;
      }
      if( i_term_off>sz_leaf ){
        rc = FTS5_CORRUPT;
      }else{
        __fts5_decode_rowid_list(&rc, &s, &a[i_off], i_term_off-i_off);
      }
      i_off = i_term_off;
      if( i_off<sz_leaf ){
        i_off += FTS5_GET_VARINT32(&a[i_off], n_keep);
      }
    }

    FTS5_BUFFER_FREE(&term);
  }else{
    fts5_buffer_t term;              /* Current term read from page */
    int sz_leaf;                   /* Offset of pgidx in a[] */
    int i_pgidx_off;
    int i_pgidx_prev = 0;           /* Previous value read from pgidx */
    int i_term_off = 0;
    int i_rowid_off = 0;
    int i_off;
    int n_doclist;

    memset(&term, 0, sizeof(fts5_buffer_t));

    if( n<4 ){
      __fts5_buffer_set(&rc, &s, 7, (const u_int8_t*)"corrupt");
      goto decode_out;
    }else{
      i_rowid_off = __fts5_get_u16(&a[0]);
      i_pgidx_off = sz_leaf = __fts5_get_u16(&a[2]);
      if( i_pgidx_off<n ){
        FTS5_GET_VARINT32(&a[i_pgidx_off], i_term_off);
      }else if( i_pgidx_off>n ){
        rc = FTS5_CORRUPT;
        goto decode_out;
      }
    }

    /* Decode the position list tail at the start of the page */
    if( i_rowid_off!=0 ){
      i_off = i_rowid_off;
    }else if( i_term_off!=0 ){
      i_off = i_term_off;
    }else{
      i_off = sz_leaf;
    }
    if( i_off>n ){
      rc = FTS5_CORRUPT;
      goto decode_out;
    }
    __fts5_decode_poslist(&rc, &s, &a[4], i_off-4);

    /* Decode any more doclist data that appears on the page before the
    ** first term. */
    n_doclist = (i_term_off ? i_term_off : sz_leaf) - i_off;
    if( n_doclist+i_off>n ){
      rc = FTS5_CORRUPT;
      goto decode_out;
    }
    __fts5_decode_doclist(&rc, &s, &a[i_off], n_doclist);

    while( i_pgidx_off<n && rc==DBSQL_OK ){
      int first = (i_pgidx_off==sz_leaf);     /* True for first term on page */
      int n_byte;                            /* Bytes of data */
      int i_end;
      
      i_pgidx_off += FTS5_GET_VARINT32(&a[i_pgidx_off], n_byte);
      i_pgidx_prev += n_byte;
      i_off = i_pgidx_prev;

      if( i_pgidx_off<n ){
        FTS5_GET_VARINT32(&a[i_pgidx_off], n_byte);
        i_end = i_pgidx_prev + n_byte;
      }else{
        i_end = sz_leaf;
      }
      if( i_end>sz_leaf ){
        rc = FTS5_CORRUPT;
        break;
      }

      if( first==0 ){
        i_off += FTS5_GET_VARINT32(&a[i_off], n_byte);
        if( n_byte>term.n ){
          rc = FTS5_CORRUPT;
          break;
        }
        term.n = n_byte;
      }
      i_off += FTS5_GET_VARINT32(&a[i_off], n_byte);
      if( i_off+n_byte>n ){
        rc = FTS5_CORRUPT;
        break;
      }
      FTS5_BUFFER_APPEND_BLOB(&rc, &term, n_byte, &a[i_off]);
      i_off += n_byte;

      __fts5_buffer_append_printf(&rc, &s, " term=");
      __fts5_buffer_append_term(&rc, &s, &term);
      i_off += __fts5_decode_doclist(&rc, &s, &a[i_off], i_end-i_off);
    }

    FTS5_BUFFER_FREE(&term);
  }
  
 decode_out:
  dbsql_free(a);
  if( rc==DBSQL_OK ){
    dbsql_result_text(ctx, (const char*)s.p, s.n, DBSQL_TRANSIENT);
  }else{
    dbsql_result_error_code(ctx, rc);
  }
  FTS5_BUFFER_FREE(&s);
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG) 
/*
** The implementation of user-defined scalar function fts5_rowid().
*/
static void __fts5_rowid_function(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args (always 2) */
  dbsql_value_t **vals           /* Function arguments */
){
  const char *arg;
  if( n_arg==0 ){
    dbsql_result_error(ctx, "should be: fts5_rowid(subject, ....)", -1);
  }else{
    arg = (const char*)dbsql_value_text(vals[0]);
    if( 0==dbsql_stricmp(arg, "segment") ){
      int64_t i_rowid;
      int segid, pgno;
      if( n_arg!=3 ){
        dbsql_result_error(ctx, 
            "should be: fts5_rowid('segment', segid, pgno))", -1
        );
      }else{
        segid = dbsql_value_int(vals[1]);
        pgno = dbsql_value_int(vals[2]);
        i_rowid = FTS5_SEGMENT_ROWID(segid, pgno);
        dbsql_result_int64(ctx, i_rowid);
      }
    }else{
      dbsql_result_error(ctx, 
        "first arg to fts5_rowid() must be 'segment'" , -1
      );
    }
  }
}
#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)

typedef struct fts5_struct_vtab_t fts5_struct_vtab_t;
struct fts5_struct_vtab_t {
  dbsql_vtab_t base;
};

typedef struct fts5_struct_vcsr_t fts5_struct_vcsr_t;
struct fts5_struct_vcsr_t {
  dbsql_vtab_cursor_t base;
  fts5_structure_t *p_struct;
  int i_level;
  int i_seg;
  int i_rowid;
};

/*
** Create a new fts5_structure() table-valued function.
*/
static int __fts5struct_connect_method(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  fts5_struct_vtab_t *p_new = 0;
  int rc = DBSQL_OK;

  rc = dbsql_declare_vtab(db, 
      "CREATE TABLE xyz("
          "level, segment, merge, segid, leaf1, leaf2, loc1, loc2, "
          "npgtombstone, nentrytombstone, nentry, struct HIDDEN);"
  );
  if( rc==DBSQL_OK ){
    p_new = __fts5_malloc_zero(&rc, sizeof(*p_new));
  }

  *vtab = (dbsql_vtab_t*)p_new;
  return rc;
}

/*
** We must have a single struct=? constraint that will be passed through
** into the filter method.  If there is no valid struct=? constraint,
** then return an DBSQL_CONSTRAINT error.
*/
static int __fts5struct_best_index_method(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;
  int rc = DBSQL_CONSTRAINT;
  struct dbsql_index_constraint_t *p;
  idx_info->estimated_cost = (double)100;
  idx_info->estimated_rows = 100;
  idx_info->idx_num = 0;
  for(i=0, p=idx_info->constraints; i<idx_info->n_constraint; i++, p++){
    if( p->usable==0 ) continue;
    if( p->op==DBSQL_INDEX_CONSTRAINT_EQ && p->i_column==11 ){
      rc = DBSQL_OK;
      idx_info->constraint_usages[i].omit = 1;
      idx_info->constraint_usages[i].argv_index = 1;
      break;
    }
  }
  return rc;
}

/*
** This method is the destructor for bytecodevtab objects.
*/
static int __fts5struct_disconnect_method(dbsql_vtab_t *vtab){
  fts5_struct_vtab_t *p = (fts5_struct_vtab_t*)vtab;
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Constructor for a new bytecodevtab_cursor object.
*/
static int __fts5struct_open_method(dbsql_vtab_t *p, dbsql_vtab_cursor_t **csr){
  int rc = DBSQL_OK;
  fts5_struct_vcsr_t *p_new = 0;

  p_new = __fts5_malloc_zero(&rc, sizeof(*p_new));
  *csr = (dbsql_vtab_cursor_t*)p_new;

  return DBSQL_OK;
}

/*
** Destructor for a bytecodevtab_cursor.
*/
static int __fts5struct_close_method(dbsql_vtab_cursor_t *cur){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t*)cur;
  __fts5_structure_release_local(csr->p_struct);
  dbsql_free(csr);
  return DBSQL_OK;
}


/*
** Advance a bytecodevtab_cursor to its next row of output.
*/
static int __fts5struct_next_method(dbsql_vtab_cursor_t *cur){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t*)cur;
  fts5_structure_t *p = csr->p_struct;

  assert( csr->p_struct );
  csr->i_seg++;
  csr->i_rowid++;
  while( csr->i_level<p->n_level && csr->i_seg>=p->levels[csr->i_level].n_seg ){
    csr->i_level++;
    csr->i_seg = 0;
  }
  if( csr->i_level>=p->n_level ){
    __fts5_structure_release_local(csr->p_struct);
    csr->p_struct = 0;
  }
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __fts5struct_eof_method(dbsql_vtab_cursor_t *cur){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t*)cur;
  return csr->p_struct==0;
}

static int __fts5struct_rowid_method(
  dbsql_vtab_cursor_t *cur, 
  int64_t *pi_rowid
){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t*)cur;
  *pi_rowid = csr->i_rowid;
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the bytecodevtab_cursor
** is currently pointing.
*/
static int __fts5struct_column_method(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t*)cur;
  fts5_structure_t *p = csr->p_struct;
  fts5_structure_segment_t *seg = &p->levels[csr->i_level].segs[csr->i_seg];

  switch( i ){
    case 0: /* level */
      dbsql_result_int(ctx, csr->i_level);
      break;
    case 1: /* segment */
      dbsql_result_int(ctx, csr->i_seg);
      break;
    case 2: /* merge */
      dbsql_result_int(ctx, csr->i_seg < p->levels[csr->i_level].n_merge);
      break;
    case 3: /* segid */
      dbsql_result_int(ctx, seg->i_segid);
      break;
    case 4: /* leaf1 */
      dbsql_result_int(ctx, seg->pgno_first);
      break;
    case 5: /* leaf2 */
      dbsql_result_int(ctx, seg->pgno_last);
      break;
    case 6: /* origin1 */
      dbsql_result_int64(ctx, seg->i_origin1);
      break;
    case 7: /* origin2 */
      dbsql_result_int64(ctx, seg->i_origin2);
      break;
    case 8: /* npgtombstone */
      dbsql_result_int(ctx, seg->n_pg_tombstone);
      break;
    case 9: /* nentrytombstone */
      dbsql_result_int64(ctx, seg->n_entry_tombstone);
      break;
    case 10: /* nentry */
      dbsql_result_int64(ctx, seg->n_entry);
      break;
  }
  return DBSQL_OK;
}

/*
** Initialize a cursor.
**
**    idx_num==0     means show all subprograms
**    idx_num==1     means show only the main bytecode and omit subprograms.
*/
static int __fts5struct_filter_method(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  fts5_struct_vcsr_t *csr = (fts5_struct_vcsr_t *)vtab_cursor;
  int rc = DBSQL_OK;

  const u_int8_t *blobs = 0;
  int n_blob = 0;

  assert( argc==1 );
  __fts5_structure_release_local(csr->p_struct);
  csr->p_struct = 0;

  n_blob = dbsql_value_bytes(argv[0]);
  blobs = (const u_int8_t*)dbsql_value_blob(argv[0]);
  rc = __fts5_structure_decode(blobs, n_blob, 0, &csr->p_struct);
  if( rc==DBSQL_OK ){
    csr->i_level = 0;
    csr->i_rowid = 0;
    csr->i_seg = -1;
    rc = __fts5struct_next_method(vtab_cursor);
  }

  return rc;
}

#endif /* DBSQL_TEST || DBSQL_FTS5_DEBUG */

/*
** This is called as part of registering the FTS5 module with database
** connection db. It registers several user-defined scalar functions useful
** with FTS5.
**
** If successful, DBSQL_OK is returned. If an error occurs, some other
** dbsql error code is returned instead.
*/
int __fts5_index_init(DBSQL *db){
#if defined(DBSQL_TEST) || defined(DBSQL_FTS5_DEBUG)
  int rc = dbsql_create_function(
      db, "fts5_decode", 2, DBSQL_UTF8, 0, __fts5_decode_function, 0, 0
  );

  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(
        db, "fts5_decode_none", 2, 
        DBSQL_UTF8, (void*)db, __fts5_decode_function, 0, 0
    );
  }

  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(
        db, "fts5_rowid", -1, DBSQL_UTF8, 0, __fts5_rowid_function, 0, 0
    );
  }

  if( rc==DBSQL_OK ){
    static const dbsql_module_t fts5structure_module = {
      0,                           /* i_version      */
      0,                           /* create       */
      __fts5struct_connect_method,     /* connect      */
      __fts5struct_best_index_method,   /* best_index    */
      __fts5struct_disconnect_method,  /* disconnect   */
      0,                           /* destroy      */
      __fts5struct_open_method,        /* x_open         */
      __fts5struct_close_method,       /* x_close        */
      __fts5struct_filter_method,      /* filter       */
      __fts5struct_next_method,        /* next         */
      __fts5struct_eof_method,         /* eof          */
      __fts5struct_column_method,      /* column       */
      __fts5struct_rowid_method,       /* rowid        */
      0,                           /* update       */
      0,                           /* begin        */
      0,                           /* x_sync         */
      0,                           /* commit       */
      0,                           /* rollback     */
      0,                           /* find_function */
      0,                           /* rename       */
      0,                           /* savepoint    */
      0,                           /* release      */
      0,                           /* rollback_to   */
      0,                           /* shadow_name   */
      0                            /* integrity    */
    };
    rc = dbsql_create_module(db, "fts5_structure", &fts5structure_module, 0);
  }
  return rc;
#else
  return DBSQL_OK;
  UNUSED_PARAM(db);
#endif
}


int __fts5_index_reset(fts5_index_t *p){
  assert( p->p_struct==0 || p->i_struct_version!=0 );
  if( __fts5_index_data_version(p)!=p->i_struct_version ){
    __fts5_structure_invalidate(p);
  }
  return __fts5_index_return(p);
}
