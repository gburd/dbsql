/*
** 2011-07-09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code for the vdbe_sorter_t object, used in concert with
** a vdbe_cursor_t to sort large numbers of keys for CREATE INDEX statements
** or by SELECT statements with ORDER BY clauses that cannot be satisfied
** using indexes and without LIMIT clauses.
**
** The vdbe_sorter_t object implements a multi-threaded external merge sort
** algorithm that is efficient even if the number of elements being sorted
** exceeds the available memory.
**
** Here is the (internal, non-API) interface between this module and the
** rest of the dbsql system:
**
**    __vdbe_sorter_init()       Create a new vdbe_sorter_t object.
**
**    __vdbe_sorter_write()      Add a single new row to the vdbe_sorter_t
**                                  object.  The row is a binary blob in the
**                                  OP_MakeRecord format that contains both
**                                  the ORDER BY key columns and result columns
**                                  in the case of a SELECT w/ ORDER BY, or
**                                  the complete record for an index entry
**                                  in the case of a CREATE INDEX.
**
**    __vdbe_sorter_rewind()     Sort all content previously added.
**                                  Position the read cursor on the
**                                  first sorted element.
**
**    __vdbe_sorter_next()       Advance the read cursor to the next sorted
**                                  element.
**
**    __vdbe_sorter_rowkey()     Return the complete binary blob for the
**                                  row currently under the read cursor.
**
**    __vdbe_sorter_compare()    Compare the binary blob for the row
**                                  currently under the read cursor against
**                                  another binary blob X and report if
**                                  X is strictly less than the read cursor.
**                                  Used to enforce uniqueness in a
**                                  CREATE UNIQUE INDEX statement.
**
**    __vdbe_sorter_close()      Close the vdbe_sorter_t object and reclaim
**                                  all resources.
**
**    __vdbe_sorter_reset()      Refurbish the vdbe_sorter_t for reuse.  This
**                                  is like Close() followed by Init() only
**                                  much faster.
**
** The interfaces above must be called in a particular order.  Write() can
** only occur in between Init()/Reset() and Rewind().  Next(), Rowkey(), and
** Compare() can only occur in between Rewind() and Close()/Reset(). i.e.
**
**   Init()
**   for each record: Write()
**   Rewind()
**     Rowkey()/Compare()
**   Next()
**   Close()
**
** Algorithm:
**
** Records passed to the sorter via calls to Write() are initially held
** unsorted in main memory. Assuming the amount of memory used never exceeds
** a threshold, when Rewind() is called the set of records is sorted using
** an in-memory merge sort. In this case, no temporary files are required
** and subsequent calls to Rowkey(), Next() and Compare() read records
** directly from main memory.
**
** If the amount of space used to store records in main memory exceeds the
** threshold, then the set of records currently in memory are sorted and
** written to a temporary file in "Packed Memory Array" (PMA) format.
** A PMA created at this point is known as a "level-0 PMA". Higher levels
** of PMAs may be created by merging existing PMAs together - for example
** merging two or more level-0 PMAs together creates a level-1 PMA.
**
** The threshold for the amount of main memory to use before flushing
** records to a PMA is roughly the same as the limit configured for the
** page-cache of the main database. Specifically, the threshold is set to
** the value returned by "PRAGMA main.page_size" multiplied by
** that returned by "PRAGMA main.cache_size", in bytes.
**
** If the sorter is running in single-threaded mode, then all PMAs generated
** are appended to a single temporary file. Or, if the sorter is running in
** multi-threaded mode then up to (N+1) temporary files may be opened, where
** N is the configured number of worker threads. In this case, instead of
** sorting the records and writing the PMA to a temporary file itself, the
** calling thread usually launches a worker thread to do so. Except, if
** there are already N worker threads running, the main thread does the work
** itself.
**
** The sorter is running in multi-threaded mode if (a) the library was built
** with pre-processor symbol DBSQL_MAX_WORKER_THREADS set to a value greater
** than zero, and (b) worker threads have been enabled at runtime by calling
** "PRAGMA threads=N" with some value of N greater than 0.
**
** When Rewind() is called, any data remaining in memory is flushed to a
** final PMA. So at this point the data is stored in some number of sorted
** PMAs within temporary files on disk.
**
** If there are fewer than SORTER_MAX_MERGE_COUNT PMAs in total and the
** sorter is running in single-threaded mode, then these PMAs are merged
** incrementally as keys are retrieved from the sorter by the VDBE.  The
** merge_engine_t object, described in further detail below, performs this
** merge.
**
** Or, if running in multi-threaded mode, then a background thread is
** launched to merge the existing PMAs. Once the background thread has
** merged T bytes of data into a single sorted PMA, the main thread
** begins reading keys from that PMA while the background thread proceeds
** with merging the next T bytes of data. And so on.
**
** Parameter T is set to half the value of the memory threshold used
** by Write() above to determine when to create a new PMA.
**
** If there are more than SORTER_MAX_MERGE_COUNT PMAs in total when
** Rewind() is called, then a hierarchy of incremental-merges is used.
** First, T bytes of data from the first SORTER_MAX_MERGE_COUNT PMAs on
** disk are merged together. Then T bytes of data from the second set, and
** so on, such that no operation ever merges more than SORTER_MAX_MERGE_COUNT
** PMAs at a time. This done is to improve locality.
**
** If running in multi-threaded mode and there are more than
** SORTER_MAX_MERGE_COUNT PMAs on disk when Rewind() is called, then more
** than one background thread may be created. Specifically, there may be
** one background thread for each temporary file on disk, and one background
** thread to merge the output of each of the others to a single PMA for
** the main thread to read from.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

/*
** If DBSQL_DEBUG_SORTER_THREADS is defined, this module outputs various
** messages to stderr that may be helpful in understanding the performance
** characteristics of the sorter in multi-threaded mode.
*/
#if 0
# define DBSQL_DEBUG_SORTER_THREADS 1
#endif

/*
** Hard-coded maximum amount of data to accumulate in memory before flushing
** to a level 0 PMA. The purpose of this limit is to prevent various integer
** overflows. 512MiB.
*/
#define DBSQL_MAX_PMASZ    (1<<29)

/*
** Private objects used by the sorter
*/
typedef struct merge_engine_t merge_engine_t;     /* Merge PMAs together */
typedef struct pma_reader_t pma_reader_t;         /* Incrementally read one PMA */
typedef struct pma_writer_t pma_writer_t;         /* Incrementally write one PMA */
typedef struct sorter_record_t sorter_record_t;   /* A record being sorted */
typedef struct sort_subtask_t sort_subtask_t;     /* A sub-task in the sort process */
typedef struct sorter_file_t sorter_file_t;       /* Temporary file object wrapper */
typedef struct sorter_list_t sorter_list_t;       /* In-memory list of records */
typedef struct incr_merger_t incr_merger_t;       /* Read & merge multiple PMAs */

/*
** A container for a temp file handle and the current amount of data
** stored in the file.
*/
struct sorter_file_t {
  dbsql_file_t *fd;              /* File handle */
  int64_t i_eof;                       /* Bytes of data stored in fd */
};

/*
** An in-memory list of objects to be sorted.
**
** If memories==0 then each object is allocated separately and the objects
** are connected using sorter_record_t.u.next.  If memories!=0 then all objects
** are stored in the memories[] bulk memory, one right after the other, and
** are connected using sorter_record_t.u.i_next.
*/
struct sorter_list_t {
  sorter_record_t *list;            /* Linked list of records */
  u_int8_t *memories;                    /* If non-NULL, bulk memory to hold list */
  int64_t sz_pma;                      /* Size of list as PMA in bytes */
};

/*
** The merge_engine_t object is used to combine two or more smaller PMAs into
** one big PMA using a merge operation.  Separate PMAs all need to be
** combined into one big PMA in order to be able to step through the sorted
** records in order.
**
** The readrs[] array contains a pma_reader_t object for each of the PMAs being
** merged.  An readrs[] object either points to a valid key or else is at EOF.
** ("EOF" means "End Of File".  When readrs[] is at EOF there is no more data.)
** For the purposes of the paragraphs below, we assume that the array is
** actually N elements in size, where N is the smallest power of 2 greater
** to or equal to the number of PMAs being merged. The extra readrs[] elements
** are treated as if they are empty (always at EOF).
**
** The trees[] array is also N elements in size. The value of N is stored in
** the merge_engine_t.n_tree variable.
**
** The final (N/2) elements of trees[] contain the results of comparing
** pairs of PMA keys together. Element i contains the result of
** comparing readrs[2*i-N] and readrs[2*i-N+1]. Whichever key is smaller, the
** trees element is set to the index of it.
**
** For the purposes of this comparison, EOF is considered greater than any
** other key value. If the keys are equal (only possible with two EOF
** values), it doesn't matter which index is stored.
**
** The (N/4) elements of trees[] that precede the final (N/2) described
** above contains the index of the smallest of each block of 4 __pma_readers
** And so on. So that trees[1] contains the index of the pma_reader_t that
** currently points to the smallest key value. trees[0] is unused.
**
** Example:
**
**     readrs[0] -> Banana
**     readrs[1] -> Feijoa
**     readrs[2] -> Elderberry
**     readrs[3] -> Currant
**     readrs[4] -> Grapefruit
**     readrs[5] -> Apple
**     readrs[6] -> Durian
**     readrs[7] -> EOF
**
**     trees[] = { X, 5   0, 5    0, 3, 5, 6 }
**
** The current element is "Apple" (the value of the key indicated by
** pma_reader_t 5). When the Next() operation is invoked, pma_reader_t 5 will
** be advanced to the next key in its segment. Say the next key is
** "Eggplant":
**
**     readrs[5] -> Eggplant
**
** The contents of trees[] are updated first by comparing the new pma_reader_t
** 5 key to the current key of pma_reader_t 4 (still "Grapefruit"). The pma_reader_t
** 5 value is still smaller, so trees[6] is set to 5. And so on up the tree.
** The value of pma_reader_t 6 - "Durian" - is now smaller than that of pma_reader_t
** 5, so trees[3] is set to 6. Key 0 is smaller than key 6 (Banana<Durian),
** so the value written into element 1 of the array is 0. As follows:
**
**     trees[] = { X, 0   0, 6    0, 3, 5, 6 }
**
** In other words, each time we advance to the next sorter element, log2(N)
** key comparison operations are required, where N is the number of segments
** being merged (rounded up to the next power of 2).
*/
struct merge_engine_t {
  int n_tree;                 /* Used size of trees/readrs (power of 2) */
  sort_subtask_t *task;        /* Used by this thread only */
  int *trees;                /* Current state of incremental merge */
  pma_reader_t *readrs;         /* Array of __pma_readers to merge data from */
};

/*
** This object represents a single thread of control in a sort operation.
** Exactly vdbe_sorter_t.n_task instances of this object are allocated
** as part of each vdbe_sorter_t object. Instances are never allocated any
** other way. vdbe_sorter_t.n_task is set to the number of worker threads allowed
** (see DBSQL_CONFIG_WORKER_THREADS) plus one (the main thread).  Thus for
** single-threaded operation, there is exactly one instance of this object
** and for multi-threaded operation there are two or more instances.
**
** Essentially, this structure contains all those fields of the vdbe_sorter_t
** structure for which each thread requires a separate instance. For example,
** each thread requeries its own unpacked_record_t object to unpack records in
** as part of comparison operations.
**
** Before a background thread is launched, variable done is set to 0. Then,
** right before it exits, the thread itself sets done to 1. This is used for
** two purposes:
**
**   1. When flushing the contents of memory to a level-0 PMA on disk, to
**      attempt to select a sort_subtask_t for which there is not already an
**      active background thread (since doing so causes the main thread
**      to block until it finishes).
**
**   2. If DBSQL_DEBUG_SORTER_THREADS is defined, to determine if a call
**      to __thread_join() is likely to block. Cases that are likely to
**      block provoke debugging output.
**
** In both cases, the effects of the main thread seeing (done==0) even
** after the thread has finished are not dire. So we don't worry about
** memory barriers and such here.
*/
typedef int (*__sorter_compare)(sort_subtask_t*,int*,const void*,int,const void*,int);
struct sort_subtask_t {
  sq_lite_thread_t *thread;          /* Background thread, if any */
  int done;                      /* Set if thread is finished but not joined */
  int n_pma;                       /* Number of PMAs currently in file */
  vdbe_sorter_t *sorter;            /* Sorter that owns this sub-task */
  unpacked_record_t *unpacked;      /* Space to unpack a record */
  sorter_list_t list;                /* List for thread to write to a PMA */
  __sorter_compare compare;         /* Compare function to use */
  sorter_file_t file;                /* Temp file for level-0 PMAs */
  sorter_file_t file2;               /* Space for other PMAs */
  u_int64_t n_spill;                     /* Total bytes written by this task */
};


/*
** Main sorter structure. A single instance of this is allocated for each
** sorter cursor created by the VDBE.
**
** mx_keysize:
**   As records are added to the sorter by calls to __vdbe_sorter_write(),
**   this variable is updated so as to be set to the size on disk of the
**   largest record in the sorter.
*/
struct vdbe_sorter_t {
  int mn_pma_size;                  /* Minimum PMA size, in bytes */
  int mx_pma_size;                  /* Maximum PMA size, in bytes.  0==no limit */
  int mx_keysize;                  /* Largest serialized key seen so far */
  int pgsz;                       /* Main database page size */
  pma_reader_t *reader;             /* Readr data from here after Rewind() */
  merge_engine_t *merger;           /* Or here, if use_threads==0 */
  DBSQL *db;                    /* Database connection */
  key_info_t *key_info;              /* How to compare records */
  unpacked_record_t *unpacked;      /* Used by __vdbe_sorter_compare() */
  sorter_list_t list;                /* List of in-memory records */
  int i_memory;                    /* Offset of free space in list.memories */
  int n_memory;                    /* Size of list.memories allocation in bytes */
  u_int8_t use_pma;                     /* True if one or more PMAs created */
  u_int8_t use_threads;                 /* True to use background threads */
  u_int8_t i_prev;                       /* Previous thread used to flush PMA */
  u_int8_t n_task;                       /* Size of tasks[] array */
  u_int8_t type_mask;
  sort_subtask_t tasks[FLEXARRAY];   /* One or more subtasks */
};

/* Size (in bytes) of a vdbe_sorter_t object that works with N or fewer subtasks */
#define SZ_VDBESORTER(N)  (offsetof(vdbe_sorter_t,tasks)+(N)*sizeof(sort_subtask_t))

#define SORTER_TYPE_INTEGER 0x01
#define SORTER_TYPE_TEXT    0x02

/*
** An instance of the following object is used to read records out of a
** PMA, in sorted order.  The next key to be read is cached in n_key/keys.
** keys might point into maps or into buffers.  If neither of those locations
** contain a contiguous representation of the key, then allocs is allocated
** and the key is copied into allocs and keys is made to point to allocs.
**
** fd==0 at EOF.
*/
struct pma_reader_t {
  int64_t i_read_off;               /* Current read offset */
  int64_t i_eof;                   /* 1 byte past EOF for this pma_reader_t */
  int n_alloc;                 /* Bytes of space at allocs */
  int n_key;                   /* Number of bytes in key */
  dbsql_file_t *fd;          /* File handle we are reading from */
  u_int8_t *allocs;                 /* Space for keys if buffers and map wont work */
  u_int8_t *keys;                   /* Pointer to current key */
  u_int8_t *buffers;                /* Current read buffer */
  int n_buffer;                /* Size of read buffer in bytes */
  u_int8_t *maps;                   /* Pointer to mapping of entire file */
  incr_merger_t *incr;          /* Incremental merger */
};

/*
** Normally, a pma_reader_t object iterates through an existing PMA stored
** within a temp file. However, if the pma_reader_t.incr variable points to
** an object of the following type, it may be used to iterate/merge through
** multiple PMAs simultaneously.
**
** There are two types of incr_merger_t object - single (use_thread==0) and
** multi-threaded (use_thread==1).
**
** A multi-threaded incr_merger_t object uses two temporary files - files[0]
** and files[1]. Neither file is allowed to grow to more than mx_sz bytes in
** size. When the incr_merger_t is initialized, it reads enough data from
** merger to populate files[0]. It then sets variables within the
** corresponding pma_reader_t object to read from that file and kicks off
** a background thread to populate files[1] with the next mx_sz bytes of
** sorted record data from merger.
**
** When the pma_reader_t reaches the end of files[0], it blocks until the
** background thread has finished populating files[1]. It then exchanges
** the contents of the files[0] and files[1] variables within this structure,
** sets the pma_reader_t fields to read from the new files[0] and kicks off
** another background thread to populate the new files[1]. And so on, until
** the contents of merger are exhausted.
**
** A single-threaded incr_merger_t does not open any temporary files of its
** own. Instead, it has exclusive access to mx_sz bytes of space beginning
** at offset i_start_off of file task->file2. And instead of using a
** background thread to prepare data for the pma_reader_t, with a single
** threaded incr_merger_t the allocate part of task->file2 is "refilled" with
** keys from merger by the calling thread whenever the pma_reader_t runs out
** of data.
*/
struct incr_merger_t {
  sort_subtask_t *task;             /* Task that owns this merger */
  merge_engine_t *merger;           /* Merge engine thread reads data from */
  int64_t i_start_off;                  /* Offset to start writing file at */
  int mx_sz;                       /* Maximum bytes of data to store */
  int eof;                       /* Set to true when merge is finished */
  int use_thread;                 /* True to use a bg thread for this object */
  sorter_file_t files[2];            /* files[0] for reading, [1] for writing */
};

/*
** An instance of this object is used for writing a PMA.
**
** The PMA is written one record at a time.  Each record is of an arbitrary
** size.  But I/O is more efficient if it occurs in page-sized blocks where
** each block is aligned on a page boundary.  This object caches writes to
** the PMA so that aligned, page-size blocks are written.
*/
struct pma_writer_t {
  int fw_err;                     /* Non-zero if in an error state */
  u_int8_t *buffers;                    /* Pointer to write buffer */
  int n_buffer;                    /* Size of write buffer in bytes */
  int i_buf_start;                  /* First byte of buffer to write */
  int i_buf_end;                    /* Last byte of buffer to write */
  int64_t i_write_off;                  /* Offset of start of buffer in file */
  dbsql_file_t *fd;              /* File handle to write to */
  u_int64_t n_pma_spill;                  /* Total number of bytes written */
};

/*
** This object is the header on a single record while that record is being
** held in memory and prior to being written out as part of a PMA.
**
** How the linked list is connected depends on how memory is being managed
** by this module. If using a separate allocation for each in-memory record
** (vdbe_sorter_t.list.memories==0), then the list is always connected using the
** sorter_record_t.u.next pointers.
**
** Or, if using the single large allocation method (vdbe_sorter_t.list.memories!=0),
** then while records are being accumulated the list is linked using the
** sorter_record_t.u.i_next offset. This is because the memories[] array may
** be __dbsql_realloc()ed while records are being accumulated. Once the VM
** has finished passing records to the sorter, or when the in-memory buffer
** is full, the list is sorted. As part of the sorting process, it is
** converted to use the sorter_record_t.u.next pointers. See function
** __vdbe_sorter_sort() for details.
*/
struct sorter_record_t {
  int n_val;                       /* Size of the record in bytes */
  union {
    sorter_record_t *next;          /* Pointer to next record in list */
    int i_next;                    /* Offset within memories of next record */
  } u;
  /* The data for the record immediately follows this header */
};

/* Return a pointer to the buffer containing the record data for sorter_record_t
** object p. Should be used as if:
**
**   void *SRVAL(sorter_record_t *p) { return (void*)&p[1]; }
*/
#define SRVAL(p) ((void*)((sorter_record_t*)(p) + 1))


/* Maximum number of PMAs that a single merge_engine_t can merge */
#define SORTER_MAX_MERGE_COUNT 16

static int __vdbe_incr_swap(incr_merger_t*);
static void __vdbe_incr_free(incr_merger_t *);

/*
** Free all memory belonging to the pma_reader_t object passed as the
** argument. All structure fields are set to zero before returning.
*/
static void __vdbe_pma_reader_clear(pma_reader_t *readr){
  dbsql_free(readr->allocs);
  dbsql_free(readr->buffers);
  if( readr->maps ) __dbsql_os_unfetch(readr->fd, 0, readr->maps);
  __vdbe_incr_free(readr->incr);
  memset(readr, 0, sizeof(pma_reader_t));
}

/*
** Read the next n_byte bytes of data from the PMA p.
** If successful, set *out to point to a buffer containing the data
** and return DBSQL_OK. Otherwise, if an error occurs, return an dbsql
** error code.
**
** The buffer returned in *out is only valid until the
** next call to this function.
*/
static int __vdbe_pma_read_blob(
  pma_reader_t *p,                   /* pma_reader_t from which to take the blob */
  int n_byte,                      /* Bytes of data to read */
  u_int8_t **out                      /* OUT: Pointer to buffer containing data */
){
  int i_buf;                       /* Offset within buffer to read from */
  int n_avail;                     /* Bytes of data available in buffer */

  if( p->maps ){
    *out = &p->maps[p->i_read_off];
    p->i_read_off += n_byte;
    return DBSQL_OK;
  }

  assert( p->buffers );

  /* If there is no more data to be read from the buffer, read the next
  ** p->n_buffer bytes of data from the file into it. Or, if there are less
  ** than p->n_buffer bytes remaining in the PMA, read all remaining data.  */
  i_buf = p->i_read_off % p->n_buffer;
  if( i_buf==0 ){
    int n_read;                    /* Bytes to read from disk */
    int rc;                       /* __dbsql_os_read() return code */

    /* Determine how many bytes of data to read. */
    if( (p->i_eof - p->i_read_off) > (int64_t)p->n_buffer ){
      n_read = p->n_buffer;
    }else{
      n_read = (int)(p->i_eof - p->i_read_off);
    }
    assert( n_read>0 );

    /* Readr data from the file. Return early if an error occurs. */
    rc = __dbsql_os_read(p->fd, p->buffers, n_read, p->i_read_off);
    assert( rc!=DBSQL_IOERR_SHORT_READ );
    if( rc!=DBSQL_OK ) return rc;
  }
  n_avail = p->n_buffer - i_buf;

  if( n_byte<=n_avail ){
    /* The requested data is available in the in-memory buffer. In this
    ** case there is no need to make a copy of the data, just return a
    ** pointer into the buffer to the caller.  */
    *out = &p->buffers[i_buf];
    p->i_read_off += n_byte;
  }else{
    /* The requested data is not all available in the in-memory buffer.
    ** In this case, allocate space at p->allocs[] to copy the requested
    ** range into. Then return a copy of pointer p->allocs to the caller.  */
    int n_rem;                     /* Bytes remaining to copy */

    /* Extend the p->allocs[] allocation if required. */
    if( p->n_alloc<n_byte ){
      u_int8_t *news;
      int64_t n_new = MAX(128, 2*(int64_t)p->n_alloc);
      while( n_byte>n_new ) n_new = n_new*2;
      news = __dbsql_realloc(p->allocs, n_new);
      if( !news ) return DBSQL_NOMEM_BKPT;
      p->n_alloc = n_new;
      p->allocs = news;
    }

    /* Copy as much data as is available in the buffer into the start of
    ** p->allocs[].  */
    memcpy(p->allocs, &p->buffers[i_buf], n_avail);
    p->i_read_off += n_avail;
    n_rem = n_byte - n_avail;

    /* The following loop copies up to p->n_buffer bytes per iteration into
    ** the p->allocs[] buffer.  */
    while( n_rem>0 ){
      int rc;                     /* __vdbe_pma_read_blob() return code */
      int n_copy;                  /* Number of bytes to copy */
      u_int8_t *nexts = 0;              /* Pointer to buffer to copy data from */

      n_copy = n_rem;
      if( n_rem>p->n_buffer ) n_copy = p->n_buffer;
      rc = __vdbe_pma_read_blob(p, n_copy, &nexts);
      if( rc!=DBSQL_OK ) return rc;
      assert( nexts!=p->allocs );
      assert( nexts!=0 );
      memcpy(&p->allocs[n_byte - n_rem], nexts, n_copy);
      n_rem -= n_copy;
    }

    *out = p->allocs;
  }

  return DBSQL_OK;
}

/*
** Read a varint from the stream of data accessed by p. Set *pn_out to
** the value read.
*/
static int __vdbe_pma_read_varint(pma_reader_t *p, u_int64_t *pn_out){
  int i_buf;

  if( p->maps ){
    p->i_read_off += __get_varint(&p->maps[p->i_read_off], pn_out);
  }else{
    i_buf = p->i_read_off % p->n_buffer;
    if( i_buf && (p->n_buffer-i_buf)>=9 ){
      p->i_read_off += __get_varint(&p->buffers[i_buf], pn_out);
    }else{
      u_int8_t varints[16], *a;
      int i = 0, rc;
      do{
        rc = __vdbe_pma_read_blob(p, 1, &a);
        if( rc ) return rc;
        varints[(i++)&0xf] = a[0];
      }while( (a[0]&0x80)!=0 );
      __get_varint(varints, pn_out);
    }
  }

  return DBSQL_OK;
}

/*
** Attempt to memory map file file. If successful, set *pp to point to the
** new mapping and return DBSQL_OK. If the mapping is not attempted
** (because the file is too large or the VFS layer is configured not to use
** mmap), return DBSQL_OK and set *pp to NULL.
**
** Or, if an error occurs, return an dbsql error code. The final value of
** *pp is undefined in this case.
*/
static int __vdbe_sorter_map_file(sort_subtask_t *task, sorter_file_t *file, u_int8_t **pp){
  int rc = DBSQL_OK;
  if( file->i_eof<=(int64_t)(task->sorter->db->n_max_sorter_mmap) ){
    dbsql_file_t *fd = file->fd;
    if( fd->methods->i_version>=3 ){
      rc = __dbsql_os_fetch(fd, 0, (int)file->i_eof, (void**)pp);
      testcase( rc!=DBSQL_OK );
    }
  }
  return rc;
}

/*
** Attach pma_reader_t readr to file file (if it is not already attached to
** that file) and seek it to offset i_off within the file.  Return DBSQL_OK
** if successful, or an dbsql error code if an error occurs.
*/
static int __vdbe_pma_reader_seek(
  sort_subtask_t *task,             /* Task context */
  pma_reader_t *readr,              /* Reader whose cursor is to be moved */
  sorter_file_t *file,              /* Sorter file to read from */
  int64_t i_off                        /* Offset in file */
){
  int rc = DBSQL_OK;

  assert( readr->incr==0 || readr->incr->eof==0 );

  if( __fault_sim(201) ) return DBSQL_IOERR_READ;
  if( readr->maps ){
    __dbsql_os_unfetch(readr->fd, 0, readr->maps);
    readr->maps = 0;
  }
  readr->i_read_off = i_off;
  readr->i_eof = file->i_eof;
  readr->fd = file->fd;

  rc = __vdbe_sorter_map_file(task, file, &readr->maps);
  if( rc==DBSQL_OK && readr->maps==0 ){
    int pgsz = task->sorter->pgsz;
    int i_buf = readr->i_read_off % pgsz;
    if( readr->buffers==0 ){
      readr->buffers = (u_int8_t*)__dbsql_malloc(pgsz);
      if( readr->buffers==0 ) rc = DBSQL_NOMEM_BKPT;
      readr->n_buffer = pgsz;
    }
    if( rc==DBSQL_OK && i_buf ){
      int n_read = pgsz - i_buf;
      if( (readr->i_read_off + n_read) > readr->i_eof ){
        n_read = (int)(readr->i_eof - readr->i_read_off);
      }
      rc = __dbsql_os_read(
          readr->fd, &readr->buffers[i_buf], n_read, readr->i_read_off
      );
      testcase( rc!=DBSQL_OK );
    }
  }

  return rc;
}

/*
** Advance pma_reader_t readr to the next key in its PMA. Return DBSQL_OK if
** no error occurs, or an dbsql error code if one does.
*/
static int __vdbe_pma_reader_next(pma_reader_t *readr){
  int rc = DBSQL_OK;             /* Return Code */
  u_int64_t n_rec = 0;                   /* Size of record in bytes */


  if( readr->i_read_off>=readr->i_eof ){
    incr_merger_t *incr = readr->incr;
    int eof = 1;
    if( incr ){
      rc = __vdbe_incr_swap(incr);
      if( rc==DBSQL_OK && incr->eof==0 ){
        rc = __vdbe_pma_reader_seek(
            incr->task, readr, &incr->files[0], incr->i_start_off
        );
        eof = 0;
      }
    }

    if( eof ){
      /* This is an EOF condition */
      __vdbe_pma_reader_clear(readr);
      testcase( rc!=DBSQL_OK );
      return rc;
    }
  }

  if( rc==DBSQL_OK ){
    rc = __vdbe_pma_read_varint(readr, &n_rec);
  }
  if( rc==DBSQL_OK ){
    readr->n_key = (int)n_rec;
    rc = __vdbe_pma_read_blob(readr, (int)n_rec, &readr->keys);
    testcase( rc!=DBSQL_OK );
  }

  return rc;
}

/*
** Initialize pma_reader_t readr to scan through the PMA stored in file file
** starting at offset i_start and ending at offset i_eof-1. This function
** leaves the pma_reader_t pointing to the first key in the PMA (or EOF if the
** PMA is empty).
**
** If the pn_byte parameter is NULL, then it is assumed that the file
** contains a single PMA, and that that PMA omits the initial length varint.
*/
static int __vdbe_pma_reader_init(
  sort_subtask_t *task,             /* Task context */
  sorter_file_t *file,              /* Sorter file to read from */
  int64_t i_start,                     /* Start offset in file */
  pma_reader_t *readr,              /* pma_reader_t to populate */
  int64_t *pn_byte                     /* IN/OUT: Increment this value by PMA size */
){
  int rc;

  assert( file->i_eof>i_start );
  assert( readr->allocs==0 && readr->n_alloc==0 );
  assert( readr->buffers==0 );
  assert( readr->maps==0 );

  rc = __vdbe_pma_reader_seek(task, readr, file, i_start);
  if( rc==DBSQL_OK ){
    u_int64_t n_byte = 0;                 /* Size of PMA in bytes */
    rc = __vdbe_pma_read_varint(readr, &n_byte);
    readr->i_eof = readr->i_read_off + n_byte;
    *pn_byte += n_byte;
  }

  if( rc==DBSQL_OK ){
    rc = __vdbe_pma_reader_next(readr);
  }
  return rc;
}

/*
** A version of __vdbe_sorter_compare_local() that assumes that it has already been
** determined that the first field of key1 is equal to the first field of
** key2.
*/
static int __vdbe_sorter_compare_tail(
  sort_subtask_t *task,             /* Subtask context (for key_info) */
  int *pb_key2_cached,              /* True if task->unpacked is key2 */
  const void *key1, int n_key1,   /* Left side of comparison */
  const void *key2, int n_key2    /* Right side of comparison */
){
  unpacked_record_t *r2 = task->unpacked;
  if( *pb_key2_cached==0 ){
    __vdbe_record_unpack(n_key2, key2, r2);
    *pb_key2_cached = 1;
  }
  return __vdbe_record_compare_with_skip(n_key1, key1, r2, 1);
}

/*
** Compare key1 (buffer key1, size n_key1 bytes) with key2 (buffer key2,
** size n_key2 bytes). Use (task->key_info) for the collation sequences
** used by the comparison. Return the result of the comparison.
**
** If IN/OUT parameter *pb_key2_cached is true when this function is called,
** it is assumed that (task->unpacked) contains the unpacked version
** of key2. If it is false, (task->unpacked) is populated with the unpacked
** version of key2 and *pb_key2_cached set to true before returning.
**
** If an OOM error is encountered, (task->unpacked->error_rc) is set
** to DBSQL_NOMEM.
*/
static int __vdbe_sorter_compare_local(
  sort_subtask_t *task,             /* Subtask context (for key_info) */
  int *pb_key2_cached,              /* True if task->unpacked is key2 */
  const void *key1, int n_key1,   /* Left side of comparison */
  const void *key2, int n_key2    /* Right side of comparison */
){
  unpacked_record_t *r2 = task->unpacked;
  if( !*pb_key2_cached ){
    __vdbe_record_unpack(n_key2, key2, r2);
    *pb_key2_cached = 1;
  }
  return __vdbe_record_compare(n_key1, key1, r2);
}

/*
** A specially optimized version of __vdbe_sorter_compare_local() that assumes that
** the first field of each key is a TEXT value and that the collation
** sequence to compare them with is BINARY.
*/
static int __vdbe_sorter_compare_text(
  sort_subtask_t *task,             /* Subtask context (for key_info) */
  int *pb_key2_cached,              /* True if task->unpacked is key2 */
  const void *key1, int n_key1,   /* Left side of comparison */
  const void *key2, int n_key2    /* Right side of comparison */
){
  const u_int8_t * const p1 = (const u_int8_t * const)key1;
  const u_int8_t * const p2 = (const u_int8_t * const)key2;
  const u_int8_t * const v1 = &p1[ p1[0] ];   /* Pointer to value 1 */
  const u_int8_t * const v2 = &p2[ p2[0] ];   /* Pointer to value 2 */

  int n1;
  int n2;
  int res;

  GET_VARINT32_NR(&p1[1], n1);
  GET_VARINT32_NR(&p2[1], n2);
  res = memcmp(v1, v2, (MIN(n1, n2) - 13)/2);
  if( res==0 ){
    res = n1 - n2;
  }

  if( res==0 ){
    if( task->sorter->key_info->n_key_field>1 ){
      res = __vdbe_sorter_compare_tail(
          task, pb_key2_cached, key1, n_key1, key2, n_key2
      );
    }
  }else{
    assert( task->sorter->key_info->sort_flags!=0 );
    assert( !(task->sorter->key_info->sort_flags[0]&KEYINFO_ORDER_BIGNULL) );
    if( task->sorter->key_info->sort_flags[0] ){
      res = res * -1;
    }
  }

  return res;
}

/*
** A specially optimized version of __vdbe_sorter_compare_local() that assumes that
** the first field of each key is an INTEGER value.
*/
static int __vdbe_sorter_compare_int(
  sort_subtask_t *task,             /* Subtask context (for key_info) */
  int *pb_key2_cached,              /* True if task->unpacked is key2 */
  const void *key1, int n_key1,   /* Left side of comparison */
  const void *key2, int n_key2    /* Right side of comparison */
){
  const u_int8_t * const p1 = (const u_int8_t * const)key1;
  const u_int8_t * const p2 = (const u_int8_t * const)key2;
  const int s1 = p1[1];                 /* Left hand serial type */
  const int s2 = p2[1];                 /* Right hand serial type */
  const u_int8_t * const v1 = &p1[ p1[0] ];   /* Pointer to value 1 */
  const u_int8_t * const v2 = &p2[ p2[0] ];   /* Pointer to value 2 */
  int res;                              /* Return value */

  assert( (s1>0 && s1<7) || s1==8 || s1==9 );
  assert( (s2>0 && s2<7) || s2==8 || s2==9 );

  if( s1==s2 ){
    /* The two values have the same sign. Compare using memcmp(). */
    static const u_int8_t lens[] = {0, 1, 2, 3, 4, 6, 8, 0, 0, 0 };
    const u_int8_t n = lens[s1];
    int i;
    res = 0;
    for(i=0; i<n; i++){
      if( (res = v1[i] - v2[i])!=0 ){
        if( ((v1[0] ^ v2[0]) & 0x80)!=0 ){
          res = v1[0] & 0x80 ? -1 : +1;
        }
        break;
      }
    }
  }else if( s1>7 && s2>7 ){
    res = s1 - s2;
  }else{
    if( s2>7 ){
      res = +1;
    }else if( s1>7 ){
      res = -1;
    }else{
      res = s1 - s2;
    }
    assert( res!=0 );

    if( res>0 ){
      if( *v1 & 0x80 ) res = -1;
    }else{
      if( *v2 & 0x80 ) res = +1;
    }
  }

  assert( task->sorter->key_info->sort_flags!=0 );
  if( res==0 ){
    if( task->sorter->key_info->n_key_field>1 ){
      res = __vdbe_sorter_compare_tail(
          task, pb_key2_cached, key1, n_key1, key2, n_key2
      );
    }
  }else if( task->sorter->key_info->sort_flags[0] ){
    assert( !(task->sorter->key_info->sort_flags[0]&KEYINFO_ORDER_BIGNULL) );
    res = res * -1;
  }

  return res;
}

/*
** Initialize the temporary index cursor just opened as a sorter cursor.
**
** Usually, the sorter module uses the value of (csr->key_info->n_key_field)
** to determine the number of fields that should be compared from the
** records being sorted. However, if the value passed as argument n_field
** is non-zero and the sorter is able to guarantee a stable sort, n_field
** is used instead. This is used when sorting records for a CREATE INDEX
** statement. In this case, keys are always delivered to the sorter in
** order of the primary key, which happens to be make up the final part
** of the records being sorted. So if the sort is stable, there is never
** any reason to compare PK fields and they can be ignored for a small
** performance boost.
**
** The sorter can guarantee a stable sort when running in single-threaded
** mode, but not in multi-threaded mode.
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
*/
int __vdbe_sorter_init(
  DBSQL *db,                    /* Database connection (for malloc()) */
  int n_field,                     /* Number of key fields in each record */
  vdbe_cursor_t *csr                /* Cursor that holds the new sorter */
){
  int pgsz;                       /* Page size of main database */
  int i;                          /* Used to iterate through tasks[] */
  vdbe_sorter_t *sorter;            /* The new sorter */
  key_info_t *key_info;              /* Copy of csr->key_info with db==0 */
  int sz_key_info;                  /* Size of csr->key_info in bytes */
  int64_t sz;                         /* Size of sorter in bytes */
  int rc = DBSQL_OK;
#if DBSQL_MAX_WORKER_THREADS==0
# define N_WORKER 0
#else
  int N_WORKER;
#endif

  /* Initialize the upper limit on the number of worker threads */
#if DBSQL_MAX_WORKER_THREADS>0
  if( __temp_in_memory(db) || __global_config.core_mutex==0 ){
    N_WORKER = 0;
  }else{
    N_WORKER = db->limits[DBSQL_LIMIT_WORKER_THREADS];
  }
#endif

  /* Do not allow the total number of threads (main thread + all workers)
  ** to exceed the maximum merge count */
#if DBSQL_MAX_WORKER_THREADS>=SORTER_MAX_MERGE_COUNT
  if( N_WORKER>=SORTER_MAX_MERGE_COUNT ){
    N_WORKER = SORTER_MAX_MERGE_COUNT-1;
  }
#endif

  assert( csr->key_info );
  assert( !csr->is_ephemeral );
  assert( csr->cur_type==CURTYPE_SORTER );
  assert( sizeof(key_info_t) + UMXV(csr->key_info->n_key_field)*sizeof(coll_seq_t*)
               < 0x7fffffff );
  assert( csr->key_info->n_key_field<=csr->key_info->n_all_field );
  sz_key_info = SZ_KEYINFO(csr->key_info->n_all_field);
  sz = SZ_VDBESORTER(N_WORKER+1);

  sorter = (vdbe_sorter_t*)__db_malloc_zero(db, sz + sz_key_info);
  csr->uc.sorter = sorter;
  if( sorter==0 ){
    rc = DBSQL_NOMEM_BKPT;
  }else{
    sm_t *bt = db->dbs[0].bt;
    sorter->key_info = key_info = (key_info_t*)((u_int8_t*)sorter + sz);
    memcpy(key_info, csr->key_info, sz_key_info);
    key_info->db = 0;
    if( n_field && N_WORKER==0 ){
      key_info->n_key_field = n_field;
      assert( n_field<=csr->key_info->n_all_field );
    }
    /* It is OK that key_info reuses the sort_flags field from csr->key_info,
    ** since the csr->key_info->sort_flags[] array is invariant and lives
    ** longer that sorter. */
    assert( key_info->sort_flags==csr->key_info->sort_flags );
    __sm_enter(bt);
    sorter->pgsz = pgsz = __sm_get_page_size(bt);
    __sm_leave(bt);
    sorter->n_task = N_WORKER + 1;
    sorter->i_prev = (u_int8_t)(N_WORKER - 1);
    sorter->use_threads = (sorter->n_task>1);
    sorter->db = db;
    for(i=0; i<sorter->n_task; i++){
      sort_subtask_t *task = &sorter->tasks[i];
      task->sorter = sorter;
    }

    if( !__temp_in_memory(db) ){
      int64_t mx_cache;                /* Cache size in bytes*/
      u_int32_t sz_pma = __global_config.sz_pma;
      sorter->mn_pma_size = sz_pma * pgsz;

      mx_cache = db->dbs[0].schema->cache_size;
      if( mx_cache<0 ){
        /* A negative cache-size value C indicates that the cache is abs(C)
        ** __ki_b in size.  */
        mx_cache = mx_cache * -1024;
      }else{
        mx_cache = mx_cache * pgsz;
      }
      mx_cache = MIN(mx_cache, DBSQL_MAX_PMASZ);
      sorter->mx_pma_size = MAX(sorter->mn_pma_size, (int)mx_cache);

      /* Avoid large memory allocations if the application has requested
      ** DBSQL_CONFIG_SMALL_MALLOC. */
      if( __global_config.small_malloc==0 ){
        assert( sorter->i_memory==0 );
        sorter->n_memory = pgsz;
        sorter->list.memories = (u_int8_t*)__dbsql_malloc(pgsz);
        if( !sorter->list.memories ) rc = DBSQL_NOMEM_BKPT;
      }
    }

    if( key_info->n_all_field<13
     && (key_info->colls[0]==0 || key_info->colls[0]==db->dflt_coll)
     && (key_info->sort_flags[0] & KEYINFO_ORDER_BIGNULL)==0
    ){
      sorter->type_mask = SORTER_TYPE_INTEGER | SORTER_TYPE_TEXT;
    }
  }

  return rc;
}
#undef N_WORKER   /* Defined at the top of this function */

/*
** Free the list of sorted records starting at record.
*/
static void __vdbe_sorter_record_free(DBSQL *db, sorter_record_t *record){
  sorter_record_t *p;
  sorter_record_t *next;
  for(p=record; p; p=next){
    next = p->u.next;
    __dbsql_db_free(db, p);
  }
}

/*
** Free all resources owned by the object indicated by argument task. All
** fields of *task are zeroed before returning.
*/
static void __vdbe_sort_subtask_cleanup(DBSQL *db, sort_subtask_t *task){
  __dbsql_db_free(db, task->unpacked);
#if DBSQL_MAX_WORKER_THREADS>0
  /* task->list.memories can only be non-zero if it was handed memory
  ** from the main thread.  That only occurs DBSQL_MAX_WORKER_THREADS>0 */
  if( task->list.memories ){
    dbsql_free(task->list.memories);
  }else
#endif
  {
    assert( task->list.memories==0 );
    __vdbe_sorter_record_free(0, task->list.list);
  }
  if( task->file.fd ){
    __dbsql_os_close_free(task->file.fd);
  }
  if( task->file2.fd ){
    __dbsql_os_close_free(task->file2.fd);
  }
  memset(task, 0, sizeof(sort_subtask_t));
}

#ifdef DBSQL_DEBUG_SORTER_THREADS
static void VDBE_SORTER_WORK_DEBUG(sort_subtask_t *task, const char *event){
  int64_t t;
  int i_task = (task - task->sorter->tasks);
  __dbsql_os_current_time_int64(task->sorter->db->vfs, &t);
  fprintf(stderr, "%lld:%d %s\n", t, i_task, event);
}
static void VDBE_SORTER_REWIND_DEBUG(const char *event){
  int64_t t = 0;
  dbsql_vfs_t *vfs = dbsql_vfs_find(0);
  if( ALWAYS(vfs) ) __dbsql_os_current_time_int64(vfs, &t);
  fprintf(stderr, "%lld:X %s\n", t, event);
}
static void VDBE_SORTER_POPULATE_DEBUG(
  sort_subtask_t *task,
  const char *event
){
  int64_t t;
  int i_task = (task - task->sorter->tasks);
  __dbsql_os_current_time_int64(task->sorter->db->vfs, &t);
  fprintf(stderr, "%lld:bg%d %s\n", t, i_task, event);
}
static void VDBE_SORTER_BLOCK_DEBUG(
  sort_subtask_t *task,
  int blocked,
  const char *event
){
  if( blocked ){
    int64_t t;
    __dbsql_os_current_time_int64(task->sorter->db->vfs, &t);
    fprintf(stderr, "%lld:main %s\n", t, event);
  }
}
#else
# define VDBE_SORTER_WORK_DEBUG(x,y)
# define VDBE_SORTER_REWIND_DEBUG(y)
# define VDBE_SORTER_POPULATE_DEBUG(x,y)
# define VDBE_SORTER_BLOCK_DEBUG(x,y,z)
#endif

#if DBSQL_MAX_WORKER_THREADS>0
/*
** Join thread task->thread.
*/
static int VDBE_SORTER_JOIN_THREAD(sort_subtask_t *task){
  int rc = DBSQL_OK;
  if( task->thread ){
#ifdef DBSQL_DEBUG_SORTER_THREADS
    int done = task->done;
#endif
    void *ret = DBSQL_INT_TO_PTR(DBSQL_ERROR);
    VDBE_SORTER_BLOCK_DEBUG(task, !done, "enter");
    (void)__thread_join(task->thread, &ret);
    VDBE_SORTER_BLOCK_DEBUG(task, !done, "exit");
    rc = DBSQL_PTR_TO_INT(ret);
    assert( task->done==1 );
    task->done = 0;
    task->thread = 0;
  }
  return rc;
}

/*
** Launch a background thread to run task(in).
*/
static int __vdbe_sorter_create_thread(
  sort_subtask_t *task,             /* dbsql_thread_t will use this task object */
  void *(*x_task)(void*),          /* Routine to run in a separate thread */
  void *in                       /* Argument passed into task() */
){
  assert( task->thread==0 && task->done==0 );
  return __thread_create(&task->thread, x_task, in);
}

/*
** Join all outstanding threads launched by __sorter_write() to create
** level-0 PMAs.
*/
static int VDBE_SORTER_JOIN_ALL(vdbe_sorter_t *sorter, int rcin){
  int rc = rcin;
  int i;

  /* This function is always called by the main user thread.
  **
  ** If this function is being called after __sorter_rewind() has been called,
  ** it is possible that thread sorter->tasks[sorter->n_task-1].thread
  ** is currently attempt to join one of the other threads. To avoid a race
  ** condition where this thread also attempts to join the same object, join
  ** thread sorter->tasks[sorter->n_task-1].thread first. */
  for(i=sorter->n_task-1; i>=0; i--){
    sort_subtask_t *task = &sorter->tasks[i];
    int rc2 = VDBE_SORTER_JOIN_THREAD(task);
    if( rc==DBSQL_OK ) rc = rc2;
  }
  return rc;
}
#else
# define VDBE_SORTER_JOIN_ALL(x,rcin) (rcin)
# define VDBE_SORTER_JOIN_THREAD(task) DBSQL_OK
#endif

/*
** Allocate a new merge_engine_t object capable of handling up to
** n_reader pma_reader_t inputs.
**
** n_reader is automatically rounded up to the next power of two.
** n_reader may not exceed SORTER_MAX_MERGE_COUNT even after rounding up.
*/
static merge_engine_t *__vdbe_merge_engine_new(int n_reader){
  int N = 2;                      /* Smallest power of two >= n_reader */
  int64_t n_byte;                      /* Total bytes of space to allocate */
  merge_engine_t *p_new;              /* Pointer to allocated object to return */

  assert( n_reader<=SORTER_MAX_MERGE_COUNT );

  while( N<n_reader ) N += N;
  n_byte = sizeof(merge_engine_t) + N * (sizeof(int) + sizeof(pma_reader_t));

  p_new = __fault_sim(100) ? 0 : (merge_engine_t*)__malloc_zero(n_byte);
  if( p_new ){
    p_new->n_tree = N;
    p_new->task = 0;
    p_new->readrs = (pma_reader_t*)&p_new[1];
    p_new->trees = (int*)&p_new->readrs[N];
  }
  return p_new;
}

/*
** Free the merge_engine_t object passed as the only argument.
*/
static void __vdbe_merge_engine_free(merge_engine_t *merger){
  int i;
  if( merger ){
    for(i=0; i<merger->n_tree; i++){
      __vdbe_pma_reader_clear(&merger->readrs[i]);
    }
  }
  dbsql_free(merger);
}

/*
** Free all resources associated with the incr_merger_t object indicated by
** the first argument.
*/
static void __vdbe_incr_free(incr_merger_t *incr){
  if( incr ){
#if DBSQL_MAX_WORKER_THREADS>0
    if( incr->use_thread ){
      VDBE_SORTER_JOIN_THREAD(incr->task);
      if( incr->files[0].fd ) __dbsql_os_close_free(incr->files[0].fd);
      if( incr->files[1].fd ) __dbsql_os_close_free(incr->files[1].fd);
    }
#endif
    __vdbe_merge_engine_free(incr->merger);
    dbsql_free(incr);
  }
}

/*
** Reset a sorting cursor back to its original empty state.
*/
void __vdbe_sorter_reset(DBSQL *db, vdbe_sorter_t *sorter){
  int i;
  (void)VDBE_SORTER_JOIN_ALL(sorter, DBSQL_OK);
  assert( sorter->use_threads || sorter->reader==0 );
#if DBSQL_MAX_WORKER_THREADS>0
  if( sorter->reader ){
    __vdbe_pma_reader_clear(sorter->reader);
    __dbsql_db_free(db, sorter->reader);
    sorter->reader = 0;
  }
#endif
  __vdbe_merge_engine_free(sorter->merger);
  sorter->merger = 0;
  for(i=0; i<sorter->n_task; i++){
    sort_subtask_t *task = &sorter->tasks[i];
    __vdbe_sort_subtask_cleanup(db, task);
    task->sorter = sorter;
  }
  if( sorter->list.memories==0 ){
    __vdbe_sorter_record_free(0, sorter->list.list);
  }
  sorter->list.list = 0;
  sorter->list.sz_pma = 0;
  sorter->use_pma = 0;
  sorter->i_memory = 0;
  sorter->mx_keysize = 0;
  __dbsql_db_free(db, sorter->unpacked);
  sorter->unpacked = 0;
}

/*
** Free any cursor components allocated by __vdbe_sorter_xxx routines.
*/
void __vdbe_sorter_close(DBSQL *db, vdbe_cursor_t *csr){
  vdbe_sorter_t *sorter;
  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  if( sorter ){
    /* Increment db->n_spill by the total number of bytes of data written
    ** to temp files by this sort operation.  */
    int ii;
    for(ii=0; ii<sorter->n_task; ii++){
      db->n_spill += sorter->tasks[ii].n_spill;
    }
    __vdbe_sorter_reset(db, sorter);
    dbsql_free(sorter->list.memories);
    __dbsql_db_free(db, sorter);
    csr->uc.sorter = 0;
  }
}

#if DBSQL_MAX_MMAP_SIZE>0
/*
** The first argument is a file-handle open on a temporary file. The file
** is guaranteed to be n_byte bytes or smaller in size. This function
** attempts to extend the file to n_byte bytes in size and to ensure that
** the VFS has memory mapped it.
**
** Whether or not the file does end up memory mapped of course depends on
** the specific VFS implementation.
*/
static void VDBE_SORTER_EXTEND_FILE(DBSQL *db, dbsql_file_t *fd, int64_t n_byte){
  if( n_byte<=(int64_t)(db->n_max_sorter_mmap) && fd->methods->i_version>=3 ){
    void *p = 0;
    int chunksize = 4*1024;
    __dbsql_os_file_control_hint(fd, DBSQL_FCNTL_CHUNK_SIZE, &chunksize);
    __dbsql_os_file_control_hint(fd, DBSQL_FCNTL_SIZE_HINT, &n_byte);
    __dbsql_os_fetch(fd, 0, (int)n_byte, &p);
    if( p ) __dbsql_os_unfetch(fd, 0, p);
  }
}
#else
# define VDBE_SORTER_EXTEND_FILE(x,y,z)
#endif

/*
** Allocate space for a file-handle and open a temporary file. If successful,
** set *fd to point to the malloc'd file-handle and return DBSQL_OK.
** Otherwise, set *fd to 0 and return an dbsql error code.
*/
static int __vdbe_sorter_open_temp_file(
  DBSQL *db,                    /* Database handle doing sort */
  int64_t n_extend,                    /* Attempt to extend file to this size */
  dbsql_file_t **fd
){
  int rc;
  if( __fault_sim(202) ) return DBSQL_IOERR_ACCESS;
  rc = __dbsql_os_open_malloc(db->vfs, 0, fd,
      DBSQL_OPEN_TEMP_JOURNAL |
      DBSQL_OPEN_READWRITE    | DBSQL_OPEN_CREATE |
      DBSQL_OPEN_EXCLUSIVE    | DBSQL_OPEN_DELETEONCLOSE, &rc
  );
  if( rc==DBSQL_OK ){
    int64_t max = DBSQL_MAX_MMAP_SIZE;
    __dbsql_os_file_control_hint(*fd, DBSQL_FCNTL_MMAP_SIZE, (void*)&max);
    if( n_extend>0 ){
      VDBE_SORTER_EXTEND_FILE(db, *fd, n_extend);
    }
  }
  return rc;
}

/*
** If it has not already been allocated, allocate the unpacked_record_t
** structure at task->unpacked. Return DBSQL_OK if successful (or
** if no allocation was required), or DBSQL_NOMEM otherwise.
*/
static int __vdbe_sort_alloc_unpacked(sort_subtask_t *task){
  if( task->unpacked==0 ){
    task->unpacked = __vdbe_alloc_unpacked_record(task->sorter->key_info);
    if( task->unpacked==0 ) return DBSQL_NOMEM_BKPT;
    task->unpacked->n_field = task->sorter->key_info->n_key_field;
    task->unpacked->errCode = 0;
  }
  return DBSQL_OK;
}


/*
** Merge the two sorted lists p1 and p2 into a single list.
*/
static sorter_record_t *__vdbe_sorter_merge(
  sort_subtask_t *task,             /* Calling thread context */
  sorter_record_t *p1,               /* First list to merge */
  sorter_record_t *p2                /* Second list to merge */
){
  sorter_record_t *final = 0;
  sorter_record_t **pp = &final;
  int cached = 0;

  assert( p1!=0 && p2!=0 );
  for(;;){
    int res;
    res = task->compare(
        task, &cached, SRVAL(p1), p1->n_val, SRVAL(p2), p2->n_val
    );

    if( res<=0 ){
      *pp = p1;
      pp = &p1->u.next;
      p1 = p1->u.next;
      if( p1==0 ){
        *pp = p2;
        break;
      }
    }else{
      *pp = p2;
      pp = &p2->u.next;
      p2 = p2->u.next;
      cached = 0;
      if( p2==0 ){
        *pp = p1;
        break;
      }
    }
  }
  return final;
}

/*
** Return the __sorter_compare function to compare values collected by the
** sorter object passed as the only argument.
*/
static __sorter_compare __vdbe_sorter_get_compare(vdbe_sorter_t *p){
  if( p->type_mask==SORTER_TYPE_INTEGER ){
    return __vdbe_sorter_compare_int;
  }else if( p->type_mask==SORTER_TYPE_TEXT ){
    return __vdbe_sorter_compare_text;
  }
  return __vdbe_sorter_compare_local;
}

/*
** Sort the linked list of records headed at task->list. Return
** DBSQL_OK if successful, or an dbsql error code (i.e. DBSQL_NOMEM) if
** an error occurs.
*/
static int __vdbe_sorter_sort(sort_subtask_t *task, sorter_list_t *list){
  int i;
  sorter_record_t *p;
  int rc;
  sorter_record_t *slots[64];

  rc = __vdbe_sort_alloc_unpacked(task);
  if( rc!=DBSQL_OK ) return rc;

  p = list->list;
  task->compare = __vdbe_sorter_get_compare(task->sorter);
  memset(slots, 0, sizeof(slots));

  while( p ){
    sorter_record_t *next;
    if( list->memories ){
      if( (u_int8_t*)p==list->memories ){
        next = 0;
      }else{
        assert( p->u.i_next<__malloc_size(list->memories) );
        next = (sorter_record_t*)&list->memories[p->u.i_next];
      }
    }else{
      next = p->u.next;
    }

    p->u.next = 0;
    for(i=0; slots[i]; i++){
      p = __vdbe_sorter_merge(task, p, slots[i]);
      /* ,--Each slots[] holds twice as much as the previous. So we cannot use
      ** |  up all 64 slots[] with only a 64-bit address space.
      ** v                                                                */
      assert( i<ARRAY_SIZE(slots) );
      slots[i] = 0;
    }
    slots[i] = p;
    p = next;
  }

  p = 0;
  for(i=0; i<ARRAY_SIZE(slots); i++){
    if( slots[i]==0 ) continue;
    p = p ? __vdbe_sorter_merge(task, p, slots[i]) : slots[i];
  }
  list->list = p;

  assert( task->unpacked->errCode==DBSQL_OK
       || task->unpacked->errCode==DBSQL_NOMEM
  );
  return task->unpacked->errCode;
}

/*
** Initialize a PMA-writer object.
*/
static void __vdbe_pma_writer_init(
  dbsql_file_t *fd,              /* File handle to write to */
  pma_writer_t *p,                   /* Object to populate */
  int n_buf,                       /* Buffer size */
  int64_t i_start                      /* Offset of fd to begin writing at */
){
  memset(p, 0, sizeof(pma_writer_t));
  p->buffers = (u_int8_t*)__dbsql_malloc(n_buf);
  if( !p->buffers ){
    p->fw_err = DBSQL_NOMEM_BKPT;
  }else{
    p->i_buf_end = p->i_buf_start = (i_start % n_buf);
    p->i_write_off = i_start - p->i_buf_start;
    p->n_buffer = n_buf;
    p->fd = fd;
  }
}

/*
** Write n_data bytes of data to the PMA. Return DBSQL_OK
** if successful, or an dbsql error code if an error occurs.
*/
static void __vdbe_pma_write_blob(pma_writer_t *p, u_int8_t *data, int n_data){
  int n_rem = n_data;
  while( n_rem>0 && p->fw_err==0 ){
    int n_copy = n_rem;
    if( n_copy>(p->n_buffer - p->i_buf_end) ){
      n_copy = p->n_buffer - p->i_buf_end;
    }

    memcpy(&p->buffers[p->i_buf_end], &data[n_data-n_rem], n_copy);
    p->i_buf_end += n_copy;
    if( p->i_buf_end==p->n_buffer ){
      p->fw_err = __dbsql_os_write(p->fd,
          &p->buffers[p->i_buf_start], p->i_buf_end - p->i_buf_start,
          p->i_write_off + p->i_buf_start
      );
      p->n_pma_spill += (p->i_buf_end - p->i_buf_start);
      p->i_buf_start = p->i_buf_end = 0;
      p->i_write_off += p->n_buffer;
    }
    assert( p->i_buf_end<p->n_buffer );

    n_rem -= n_copy;
  }
}

/*
** Flush any buffered data to disk and clean up the PMA-writer object.
** The results of using the PMA-writer after this call are undefined.
** Return DBSQL_OK if flushing the buffered data succeeds or is not
** required. Otherwise, return an dbsql error code.
**
** Before returning, set *pi_eof to the offset immediately following the
** last byte written to the file. Also, increment (*pn_spill) by the total
** number of bytes written to the file.
*/
static int __vdbe_pma_writer_finish(pma_writer_t *p, int64_t *pi_eof, u_int64_t *pn_spill){
  int rc;
  if( p->fw_err==0 && ALWAYS(p->buffers) && p->i_buf_end>p->i_buf_start ){
    p->fw_err = __dbsql_os_write(p->fd,
        &p->buffers[p->i_buf_start], p->i_buf_end - p->i_buf_start,
        p->i_write_off + p->i_buf_start
    );
    p->n_pma_spill += (p->i_buf_end - p->i_buf_start);
  }
  *pi_eof = (p->i_write_off + p->i_buf_end);
  *pn_spill += p->n_pma_spill;
  dbsql_free(p->buffers);
  rc = p->fw_err;
  memset(p, 0, sizeof(pma_writer_t));
  return rc;
}

/*
** Write value i_val encoded as a varint to the PMA. Return
** DBSQL_OK if successful, or an dbsql error code if an error occurs.
*/
static void __vdbe_pma_write_varint(pma_writer_t *p, u_int64_t i_val){
  int n_byte;
  u_int8_t bytes[10];
  n_byte = __put_varint(bytes, i_val);
  __vdbe_pma_write_blob(p, bytes, n_byte);
}

/*
** Write the current contents of in-memory linked-list list to a level-0
** PMA in the temp file belonging to sub-task task. Return DBSQL_OK if
** successful, or an dbsql error code otherwise.
**
** The format of a PMA is:
**
**     * A varint. This varint contains the total number of bytes of content
**       in the PMA (not including the varint itself).
**
**     * One or more records packed end-to-end in order of ascending keys.
**       Each record consists of a varint followed by a blob of data (the
**       key). The varint is the number of bytes in the blob of data.
*/
static int __vdbe_sorter_list_to_pma(sort_subtask_t *task, sorter_list_t *list){
  DBSQL *db = task->sorter->db;
  int rc = DBSQL_OK;             /* Return code */
  pma_writer_t writer;               /* Object used to write to the file */

#ifdef DBSQL_DEBUG
  /* Set i_sz to the expected size of file task->file after writing the PMA.
  ** This is used by an assert() statement at the end of this function.  */
  int64_t i_sz = list->sz_pma + __varint_len(list->sz_pma) + task->file.i_eof;
#endif

  VDBE_SORTER_WORK_DEBUG(task, "enter");
  memset(&writer, 0, sizeof(pma_writer_t));
  assert( list->sz_pma>0 );

  /* If the first temporary PMA file has not been opened, open it now. */
  if( task->file.fd==0 ){
    rc = __vdbe_sorter_open_temp_file(db, 0, &task->file.fd);
    assert( rc!=DBSQL_OK || task->file.fd );
    assert( task->file.i_eof==0 );
    assert( task->n_pma==0 );
  }

  /* Try to get the file to memory map */
  if( rc==DBSQL_OK ){
    VDBE_SORTER_EXTEND_FILE(db, task->file.fd, task->file.i_eof+list->sz_pma+9);
  }

  /* Sort the list */
  if( rc==DBSQL_OK ){
    rc = __vdbe_sorter_sort(task, list);
  }

  if( rc==DBSQL_OK ){
    sorter_record_t *p;
    sorter_record_t *next = 0;

    __vdbe_pma_writer_init(task->file.fd, &writer, task->sorter->pgsz,
                      task->file.i_eof);
    task->n_pma++;
    __vdbe_pma_write_varint(&writer, list->sz_pma);
    for(p=list->list; p; p=next){
      next = p->u.next;
      __vdbe_pma_write_varint(&writer, p->n_val);
      __vdbe_pma_write_blob(&writer, SRVAL(p), p->n_val);
      if( list->memories==0 ) dbsql_free(p);
    }
    list->list = p;
    rc = __vdbe_pma_writer_finish(&writer, &task->file.i_eof, &task->n_spill);
  }

  VDBE_SORTER_WORK_DEBUG(task, "exit");
  assert( rc!=DBSQL_OK || list->list==0 );
  assert( rc!=DBSQL_OK || task->file.i_eof==i_sz );
  return rc;
}

/*
** Advance the merge_engine_t to its next entry.
** Set *pb_eof to true there is no next entry because
** the merge_engine_t has reached the end of all its inputs.
**
** Return DBSQL_OK if successful or an error code if an error occurs.
*/
static int __vdbe_merge_engine_step(
  merge_engine_t *merger,      /* The merge engine to advance to the next row */
  int *pb_eof                 /* Set TRUE at EOF.  Set false for more content */
){
  int rc;
  int i_prev = merger->trees[1];/* index_t of pma_reader_t to advance */
  sort_subtask_t *task = merger->task;

  /* Advance the current pma_reader_t */
  rc = __vdbe_pma_reader_next(&merger->readrs[i_prev]);

  /* Update contents of trees[] */
  if( rc==DBSQL_OK ){
    int i;                      /* index_t of trees[] to recalculate */
    pma_reader_t *readr1;         /* First pma_reader_t to compare */
    pma_reader_t *readr2;         /* Second pma_reader_t to compare */
    int cached = 0;

    /* Find the first two __pma_readers to compare. The one that was just
    ** advanced (i_prev) and the one next to it in the array.  */
    readr1 = &merger->readrs[(i_prev & 0xFFFE)];
    readr2 = &merger->readrs[(i_prev | 0x0001)];

    for(i=(merger->n_tree+i_prev)/2; i>0; i=i/2){
      /* Compare readr1 and readr2. Store the result in variable i_res. */
      int i_res;
      if( readr1->fd==0 ){
        i_res = +1;
      }else if( readr2->fd==0 ){
        i_res = -1;
      }else{
        i_res = task->compare(task, &cached,
            readr1->keys, readr1->n_key, readr2->keys, readr2->n_key
        );
      }

      /* If readr1 contained the smaller value, set trees[i] to its index.
      ** Then set readr2 to the next pma_reader_t to compare to readr1. In this
      ** case there is no cache of readr2 in task->unpacked, so set
      ** key2 to point to the record belonging to readr2.
      **
      ** Alternatively, if readr2 contains the smaller of the two values,
      ** set trees[i] to its index and update readr1. If __vdbe_sorter_compare_local()
      ** was actually called above, then task->unpacked now contains
      ** a value equivalent to readr2. So set key2 to NULL to prevent
      ** __vdbe_sorter_compare_local() from decoding readr2 again.
      **
      ** If the two values were equal, then the value from the oldest
      ** PMA should be considered smaller. The vdbe_sorter_t.readrs[] array
      ** is sorted from oldest to newest, so readr1 contains older values
      ** than readr2 iff (readr1<readr2).  */
      if( i_res<0 || (i_res==0 && readr1<readr2) ){
        merger->trees[i] = (int)(readr1 - merger->readrs);
        readr2 = &merger->readrs[ merger->trees[i ^ 0x0001] ];
        cached = 0;
      }else{
        if( readr1->fd ) cached = 0;
        merger->trees[i] = (int)(readr2 - merger->readrs);
        readr1 = &merger->readrs[ merger->trees[i ^ 0x0001] ];
      }
    }
    *pb_eof = (merger->readrs[merger->trees[1]].fd==0);
  }

  return (rc==DBSQL_OK ? task->unpacked->errCode : rc);
}

#if DBSQL_MAX_WORKER_THREADS>0
/*
** The main routine for background threads that write level-0 PMAs.
*/
static void *__vdbe_sorter_flush_thread(void *ctx){
  sort_subtask_t *task = (sort_subtask_t*)ctx;
  int rc;                         /* Return code */
  assert( task->done==0 );
  rc = __vdbe_sorter_list_to_pma(task, &task->list);
  task->done = 1;
  return DBSQL_INT_TO_PTR(rc);
}
#endif /* DBSQL_MAX_WORKER_THREADS>0 */

/*
** Flush the current contents of vdbe_sorter_t.list to a new PMA, possibly
** using a background thread.
*/
static int __vdbe_sorter_flush_pma(vdbe_sorter_t *sorter){
#if DBSQL_MAX_WORKER_THREADS==0
  sorter->use_pma = 1;
  return __vdbe_sorter_list_to_pma(&sorter->tasks[0], &sorter->list);
#else
  int rc = DBSQL_OK;
  int i;
  sort_subtask_t *task = 0;    /* dbsql_thread_t context used to create new PMA */
  int N_WORKER = (sorter->n_task-1);

  /* Set the flag to indicate that at least one PMA has been written.
  ** Or will be, anyhow.  */
  sorter->use_pma = 1;

  /* select_t a sub-task to sort and flush the current list of in-memory
  ** records to disk. If the sorter is running in multi-threaded mode,
  ** round-robin between the first (sorter->n_task-1) tasks. Except, if
  ** the background thread from a sub-tasks previous turn is still running,
  ** skip it. If the first (sorter->n_task-1) sub-tasks are all still busy,
  ** fall back to using the final sub-task. The first (sorter->n_task-1)
  ** sub-tasks are preferred as they use background threads - the final
  ** sub-task uses the main thread. */
  for(i=0; i<N_WORKER; i++){
    int i_test = (sorter->i_prev + i + 1) % N_WORKER;
    task = &sorter->tasks[i_test];
    if( task->done ){
      rc = VDBE_SORTER_JOIN_THREAD(task);
    }
    if( rc!=DBSQL_OK || task->thread==0 ) break;
  }

  if( rc==DBSQL_OK ){
    if( i==N_WORKER ){
      /* Use the foreground thread for this operation */
      rc = __vdbe_sorter_list_to_pma(&sorter->tasks[N_WORKER], &sorter->list);
    }else{
      /* Launch a background thread for this operation */
      u_int8_t *mems;
      void *ctx;

      assert( task!=0 );
      assert( task->thread==0 && task->done==0 );
      assert( task->list.list==0 );
      assert( task->list.memories==0 || sorter->list.memories!=0 );

      mems = task->list.memories;
      ctx = (void*)task;
      sorter->i_prev = (u_int8_t)(task - sorter->tasks);
      task->list = sorter->list;
      sorter->list.list = 0;
      sorter->list.sz_pma = 0;
      if( mems ){
        sorter->list.memories = mems;
        sorter->n_memory = __malloc_size(mems);
      }else if( sorter->list.memories ){
        sorter->list.memories = __dbsql_malloc(sorter->n_memory);
        if( !sorter->list.memories ) return DBSQL_NOMEM_BKPT;
      }

      rc = __vdbe_sorter_create_thread(task, __vdbe_sorter_flush_thread, ctx);
    }
  }

  return rc;
#endif /* DBSQL_MAX_WORKER_THREADS!=0 */
}

/*
** Add a record to the sorter.
*/
int __vdbe_sorter_write(
  const vdbe_cursor_t *csr,         /* Sorter cursor */
  mem_t *val                       /* Memory cell containing record */
){
  vdbe_sorter_t *sorter;
  int rc = DBSQL_OK;             /* Return Code */
  sorter_record_t *p_new;             /* New list element */
  int flush;                     /* True to flush contents of memory to PMA */
  int64_t n_req;                       /* Bytes of memory required */
  int64_t n_pma;                       /* Bytes of PMA space required */
  int t;                          /* serial type of first record field */

  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  GET_VARINT32_NR((const u_int8_t*)&val->z[1], t);
  if( t>0 && t<10 && t!=7 ){
    sorter->type_mask &= SORTER_TYPE_INTEGER;
  }else if( t>10 && (t & 0x01) ){
    sorter->type_mask &= SORTER_TYPE_TEXT;
  }else{
    sorter->type_mask = 0;
  }

  assert( sorter );

  /* Figure out whether or not the current contents of memory should be
  ** flushed to a PMA before continuing. If so, do so.
  **
  ** If using the single large allocation mode (sorter->memories!=0), then
  ** flush the contents of memory to a new PMA if (a) at least one value is
  ** already in memory and (b) the new value will not fit in memory.
  **
  ** Or, if using separate allocations for each record, flush the contents
  ** of memory to a PMA if either of the following are true:
  **
  **   * The total memory allocated for the in-memory list is greater
  **     than (page-size * cache-size), or
  **
  **   * The total memory allocated for the in-memory list is greater
  **     than (page-size * 10) and __heap_nearly_full() returns true.
  */
  n_req = val->n + sizeof(sorter_record_t);
  n_pma = val->n + __varint_len(val->n);
  if( sorter->mx_pma_size ){
    if( sorter->list.memories ){
      flush = sorter->i_memory && (sorter->i_memory+n_req) > sorter->mx_pma_size;
    }else{
      flush = (
          (sorter->list.sz_pma > sorter->mx_pma_size)
       || (sorter->list.sz_pma > sorter->mn_pma_size && __heap_nearly_full())
      );
    }
    if( flush ){
      rc = __vdbe_sorter_flush_pma(sorter);
      sorter->list.sz_pma = 0;
      sorter->i_memory = 0;
      assert( rc!=DBSQL_OK || sorter->list.list==0 );
    }
  }

  sorter->list.sz_pma += n_pma;
  if( n_pma>sorter->mx_keysize ){
    sorter->mx_keysize = n_pma;
  }

  if( sorter->list.memories ){
    int n_min = sorter->i_memory + n_req;

    if( n_min>sorter->n_memory ){
      u_int8_t *news;
      int64_t n_new = 2 * (int64_t)sorter->n_memory;
      int i_list_off = -1;
      if( sorter->list.list ){
        i_list_off = (u_int8_t*)sorter->list.list - sorter->list.memories;
      }
      while( n_new < n_min ) n_new = n_new*2;
      if( n_new > sorter->mx_pma_size ) n_new = sorter->mx_pma_size;
      if( n_new < n_min ) n_new = n_min;
      news = __dbsql_realloc(sorter->list.memories, n_new);
      if( !news ) return DBSQL_NOMEM_BKPT;
      if( i_list_off>=0 ){
        sorter->list.list = (sorter_record_t*)&news[i_list_off];
      }
      sorter->list.memories = news;
      sorter->n_memory = n_new;
    }

    p_new = (sorter_record_t*)&sorter->list.memories[sorter->i_memory];
    sorter->i_memory += ROUND8(n_req);
    if( sorter->list.list ){
      p_new->u.i_next = (int)((u_int8_t*)(sorter->list.list) - sorter->list.memories);
    }
  }else{
    p_new = (sorter_record_t *)__dbsql_malloc(n_req);
    if( p_new==0 ){
      return DBSQL_NOMEM_BKPT;
    }
    p_new->u.next = sorter->list.list;
  }

  memcpy(SRVAL(p_new), val->z, val->n);
  p_new->n_val = val->n;
  sorter->list.list = p_new;

  return rc;
}

/*
** Read keys from incr->merger and populate incr->files[1]. The format
** of the data stored in files[1] is the same as that used by regular PMAs,
** except that the number-of-bytes varint is omitted from the start.
*/
static int __vdbe_incr_populate(incr_merger_t *incr){
  int rc = DBSQL_OK;
  int rc2;
  int64_t i_start = incr->i_start_off;
  sorter_file_t *out = &incr->files[1];
  sort_subtask_t *task = incr->task;
  merge_engine_t *merger = incr->merger;
  pma_writer_t writer;
  assert( incr->eof==0 );

  VDBE_SORTER_POPULATE_DEBUG(task, "enter");

  __vdbe_pma_writer_init(out->fd, &writer, task->sorter->pgsz, i_start);
  while( rc==DBSQL_OK ){
    int dummy;
    pma_reader_t *reader = &merger->readrs[ merger->trees[1] ];
    int n_key = reader->n_key;
    int64_t i_eof = writer.i_write_off + writer.i_buf_end;

    /* Check if the output file is full or if the input has been exhausted.
    ** In either case exit the loop. */
    if( reader->fd==0 ) break;
    if( (i_eof + n_key + __varint_len(n_key))>(i_start + incr->mx_sz) ) break;

    /* Write the next key to the output. */
    __vdbe_pma_write_varint(&writer, n_key);
    __vdbe_pma_write_blob(&writer, reader->keys, n_key);
    assert( incr->merger->task==task );
    rc = __vdbe_merge_engine_step(incr->merger, &dummy);
  }

  rc2 = __vdbe_pma_writer_finish(&writer, &out->i_eof, &task->n_spill);
  if( rc==DBSQL_OK ) rc = rc2;
  VDBE_SORTER_POPULATE_DEBUG(task, "exit");
  return rc;
}

#if DBSQL_MAX_WORKER_THREADS>0
/*
** The main routine for background threads that populate files[1] of
** multi-threaded incr_merger_t objects.
*/
static void *__vdbe_incr_populate_thread(void *ctx){
  incr_merger_t *incr = (incr_merger_t*)ctx;
  void *ret = DBSQL_INT_TO_PTR( __vdbe_incr_populate(incr) );
  incr->task->done = 1;
  return ret;
}

/*
** Launch a background thread to populate files[1] of incr.
*/
static int __vdbe_incr_bg_populate(incr_merger_t *incr){
  void *p = (void*)incr;
  assert( incr->use_thread );
  return __vdbe_sorter_create_thread(incr->task, __vdbe_incr_populate_thread, p);
}
#endif

/*
** This function is called when the pma_reader_t corresponding to incr has
** finished reading the contents of files[0]. Its purpose is to "refill"
** files[0] such that the pma_reader_t should start rereading it from the
** beginning.
**
** For single-threaded objects, this is accomplished by literally reading
** keys from incr->merger and repopulating files[0].
**
** For multi-threaded objects, all that is required is to wait until the
** background thread is finished (if it is not already) and then swap
** files[0] and files[1] in place. If the contents of merger have not
** been exhausted, this function also launches a new background thread
** to populate the new files[1].
**
** DBSQL_OK is returned on success, or an dbsql error code otherwise.
*/
static int __vdbe_incr_swap(incr_merger_t *incr){
  int rc = DBSQL_OK;

#if DBSQL_MAX_WORKER_THREADS>0
  if( incr->use_thread ){
    rc = VDBE_SORTER_JOIN_THREAD(incr->task);

    if( rc==DBSQL_OK ){
      sorter_file_t f0 = incr->files[0];
      incr->files[0] = incr->files[1];
      incr->files[1] = f0;
    }

    if( rc==DBSQL_OK ){
      if( incr->files[0].i_eof==incr->i_start_off ){
        incr->eof = 1;
      }else{
        rc = __vdbe_incr_bg_populate(incr);
      }
    }
  }else
#endif
  {
    rc = __vdbe_incr_populate(incr);
    incr->files[0] = incr->files[1];
    if( incr->files[0].i_eof==incr->i_start_off ){
      incr->eof = 1;
    }
  }

  return rc;
}

/*
** Allocate and return a new incr_merger_t object to read data from merger.
**
** If an OOM condition is encountered, return NULL. In this case free the
** merger argument before returning.
*/
static int __vdbe_incr_merger_new(
  sort_subtask_t *task,     /* The thread that will be using the new incr_merger_t */
  merge_engine_t *merger,   /* The merge_engine_t that the incr_merger_t will control */
  incr_merger_t **out      /* Write the new incr_merger_t here */
){
  int rc = DBSQL_OK;
  incr_merger_t *incr = *out = (incr_merger_t*)
       (__fault_sim(100) ? 0 : __malloc_zero(sizeof(*incr)));
  if( incr ){
    incr->merger = merger;
    incr->task = task;
    incr->mx_sz = MAX(task->sorter->mx_keysize+9,task->sorter->mx_pma_size/2);
    task->file2.i_eof += incr->mx_sz;
  }else{
    __vdbe_merge_engine_free(merger);
    rc = DBSQL_NOMEM_BKPT;
  }
  assert( *out!=0 || rc!=DBSQL_OK );
  return rc;
}

#if DBSQL_MAX_WORKER_THREADS>0
/*
** Set the "use-threads" flag on object incr.
*/
static void __vdbe_incr_merger_set_threads(incr_merger_t *incr){
  incr->use_thread = 1;
  incr->task->file2.i_eof -= incr->mx_sz;
}
#endif /* DBSQL_MAX_WORKER_THREADS>0 */



/*
** Recompute merger->trees[i_out] by comparing the next keys on the
** two __pma_readers that feed that entry.  Neither of the __pma_readers
** are advanced.  This routine merely does the comparison.
*/
static void __vdbe_merge_engine_compare(
  merge_engine_t *merger,  /* Merge engine containing __pma_readers to compare */
  int i_out               /* Store the result in merger->trees[i_out] */
){
  int i1;
  int i2;
  int i_res;
  pma_reader_t *p1;
  pma_reader_t *p2;

  assert( i_out<merger->n_tree && i_out>0 );

  if( i_out>=(merger->n_tree/2) ){
    i1 = (i_out - merger->n_tree/2) * 2;
    i2 = i1 + 1;
  }else{
    i1 = merger->trees[i_out*2];
    i2 = merger->trees[i_out*2+1];
  }

  p1 = &merger->readrs[i1];
  p2 = &merger->readrs[i2];

  if( p1->fd==0 ){
    i_res = i2;
  }else if( p2->fd==0 ){
    i_res = i1;
  }else{
    sort_subtask_t *task = merger->task;
    int cached = 0;
    int res;
    assert( task->unpacked!=0 );  /* from vdbe_sort_subtask_main() */
    res = task->compare(
        task, &cached, p1->keys, p1->n_key, p2->keys, p2->n_key
    );
    if( res<=0 ){
      i_res = i1;
    }else{
      i_res = i2;
    }
  }

  merger->trees[i_out] = i_res;
}

/*
** Allowed values for the mode parameter to __vdbe_merge_engine_init()
** and __vdbe_pma_reader_incr_merge_init().
**
** Only INCRINIT_NORMAL is valid in single-threaded builds (when
** DBSQL_MAX_WORKER_THREADS==0).  The other values are only used
** when there exists one or more separate worker threads.
*/
#define INCRINIT_NORMAL 0
#define INCRINIT_TASK   1
#define INCRINIT_ROOT   2

/*
** Forward reference required as the vdbe_incr_merge_init() and
** __vdbe_pma_reader_incr_init() routines are called mutually recursively when
** building a merge tree.
*/
static int __vdbe_pma_reader_incr_init(pma_reader_t *readr, int mode);

/*
** Initialize the merge_engine_t object passed as the second argument. Once this
** function returns, the first key of merged data may be read from the
** merge_engine_t object in the usual fashion.
**
** If argument mode is INCRINIT_ROOT, then it is assumed that any __incr_merge
** objects attached to the pma_reader_t objects that the merger reads from have
** already been populated, but that they have not yet populated files[0] and
** set the pma_reader_t objects up to read from it. In this case all that is
** required is to call __vdbe_pma_reader_next() on each pma_reader_t to point it at
** its first key.
**
** Otherwise, if mode is any value other than INCRINIT_ROOT, then use
** __vdbe_pma_reader_incr_merge_init() to initialize each pma_reader_t that feeds data
** to merger.
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
*/
static int __vdbe_merge_engine_init(
  sort_subtask_t *task,             /* dbsql_thread_t that will run merger */
  merge_engine_t *merger,           /* merge_engine_t to initialize */
  int mode                       /* One of the INCRINIT_XXX constants */
){
  int rc = DBSQL_OK;             /* Return code */
  int i;                          /* For looping over pma_reader_t objects */
  int n_tree;                      /* Number of subtrees to merge */

  /* Failure to allocate the merge would have been detected prior to
  ** invoking this routine */
  assert( merger!=0 );

  /* mode is always INCRINIT_NORMAL in single-threaded mode */
  assert( DBSQL_MAX_WORKER_THREADS>0 || mode==INCRINIT_NORMAL );

  /* Verify that the merge_engine_t is assigned to a single thread */
  assert( merger->task==0 );
  merger->task = task;

  n_tree = merger->n_tree;
  for(i=0; i<n_tree; i++){
    if( DBSQL_MAX_WORKER_THREADS>0 && mode==INCRINIT_ROOT ){
      /* __pma_readers should be normally initialized in order, as if they are
      ** reading from the same temp file this makes for more linear file IO.
      ** However, in the INCRINIT_ROOT case, if pma_reader_t readrs[n_task-1] is
      ** in use it will block the __vdbe_pma_reader_next() call while it uses
      ** the main thread to fill its buffer. So calling __pma_reader_next()
      ** on this pma_reader_t before any of the multi-threaded __pma_readers takes
      ** better advantage of multi-processor hardware. */
      rc = __vdbe_pma_reader_next(&merger->readrs[n_tree-i-1]);
    }else{
      rc = __vdbe_pma_reader_incr_init(&merger->readrs[i], INCRINIT_NORMAL);
    }
    if( rc!=DBSQL_OK ) return rc;
  }

  for(i=merger->n_tree-1; i>0; i--){
    __vdbe_merge_engine_compare(merger, i);
  }
  return task->unpacked->errCode;
}

/*
** The pma_reader_t passed as the first argument is guaranteed to be an
** incremental-reader (readr->incr!=0). This function serves to open
** and/or initialize the temp file related fields of the __incr_merge
** object at (readr->incr).
**
** If argument mode is set to INCRINIT_NORMAL, then all __pma_readers
** in the sub-tree headed by readr are also initialized. Data is then
** loaded into the buffers belonging to readr and it is set to point to
** the first key in its range.
**
** If argument mode is set to INCRINIT_TASK, then readr is guaranteed
** to be a multi-threaded pma_reader_t and this function is being called in a
** background thread. In this case all __pma_readers in the sub-tree are
** initialized as for INCRINIT_NORMAL and the files[1] buffer belonging to
** readr is populated. However, readr itself is not set up to point
** to its first key. A call to __vdbe_pma_reader_next() is still required to do
** that.
**
** The reason this function does not call __vdbe_pma_reader_next() immediately
** in the INCRINIT_TASK case is that __vdbe_pma_reader_next() assumes that it has
** to block on thread (task->thread) before accessing files[1]. But, since
** this entire function is being run by thread (task->thread), that will
** lead to the current background thread attempting to join itself.
**
** Finally, if argument mode is set to INCRINIT_ROOT, it may be assumed
** that readr->incr is a multi-threaded __incr_merge objects, and that all
** child-trees have already been initialized using __incr_init(INCRINIT_TASK).
** In this case __vdbe_pma_reader_next() is called on all child __pma_readers and
** the current pma_reader_t set to point to the first key in its range.
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
*/
static int __vdbe_pma_reader_incr_merge_init(pma_reader_t *readr, int mode){
  int rc = DBSQL_OK;
  incr_merger_t *incr = readr->incr;
  sort_subtask_t *task = incr->task;
  DBSQL *db = task->sorter->db;

  /* mode is always INCRINIT_NORMAL in single-threaded mode */
  assert( DBSQL_MAX_WORKER_THREADS>0 || mode==INCRINIT_NORMAL );

  rc = __vdbe_merge_engine_init(task, incr->merger, mode);

  /* Set up the required files for incr. A multi-threaded __incr_merge object
  ** requires two temp files to itself, whereas a single-threaded object
  ** only requires a region of task->file2. */
  if( rc==DBSQL_OK ){
    int mx_sz = incr->mx_sz;
#if DBSQL_MAX_WORKER_THREADS>0
    if( incr->use_thread ){
      rc = __vdbe_sorter_open_temp_file(db, mx_sz, &incr->files[0].fd);
      if( rc==DBSQL_OK ){
        rc = __vdbe_sorter_open_temp_file(db, mx_sz, &incr->files[1].fd);
      }
    }else
#endif
    /*if( !incr->use_thread )*/{
      if( task->file2.fd==0 ){
        assert( task->file2.i_eof>0 );
        rc = __vdbe_sorter_open_temp_file(db, task->file2.i_eof, &task->file2.fd);
        task->file2.i_eof = 0;
      }
      if( rc==DBSQL_OK ){
        incr->files[1].fd = task->file2.fd;
        incr->i_start_off = task->file2.i_eof;
        task->file2.i_eof += mx_sz;
      }
    }
  }

#if DBSQL_MAX_WORKER_THREADS>0
  if( rc==DBSQL_OK && incr->use_thread ){
    /* Use the current thread to populate files[1], even though this
    ** pma_reader_t is multi-threaded. If this is an INCRINIT_TASK object,
    ** then this function is already running in background thread
    ** incr->task->thread.
    **
    ** If this is the INCRINIT_ROOT object, then it is running in the
    ** main VDBE thread. But that is Ok, as that thread cannot return
    ** control to the VDBE or proceed with anything useful until the
    ** first results are ready from this merger object anyway.
    */
    assert( mode==INCRINIT_ROOT || mode==INCRINIT_TASK );
    rc = __vdbe_incr_populate(incr);
  }
#endif

  if( rc==DBSQL_OK && (DBSQL_MAX_WORKER_THREADS==0 || mode!=INCRINIT_TASK) ){
    rc = __vdbe_pma_reader_next(readr);
  }

  return rc;
}

#if DBSQL_MAX_WORKER_THREADS>0
/*
** The main routine for __vdbe_pma_reader_incr_merge_init() operations run in
** background threads.
*/
static void *__vdbe_pma_reader_bg_incr_init(void *ctx){
  pma_reader_t *reader = (pma_reader_t*)ctx;
  void *ret = DBSQL_INT_TO_PTR(
                  __vdbe_pma_reader_incr_merge_init(reader,INCRINIT_TASK)
               );
  reader->incr->task->done = 1;
  return ret;
}
#endif

/*
** If the pma_reader_t passed as the first argument is not an incremental-reader
** (if readr->incr==0), then this function is a no-op. Otherwise, it invokes
** the __vdbe_pma_reader_incr_merge_init() function with the parameters passed to
** this routine to initialize the incremental merge.
**
** If the incr_merger_t object is multi-threaded (incr_merger_t.use_thread==1),
** then a background thread is launched to call __vdbe_pma_reader_incr_merge_init().
** Or, if the incr_merger_t is single threaded, the same function is called
** using the current thread.
*/
static int __vdbe_pma_reader_incr_init(pma_reader_t *readr, int mode){
  incr_merger_t *incr = readr->incr;   /* Incremental merger */
  int rc = DBSQL_OK;                  /* Return code */
  if( incr ){
#if DBSQL_MAX_WORKER_THREADS>0
    assert( incr->use_thread==0 || mode==INCRINIT_TASK );
    if( incr->use_thread ){
      void *ctx = (void*)readr;
      rc = __vdbe_sorter_create_thread(incr->task, __vdbe_pma_reader_bg_incr_init, ctx);
    }else
#endif
    {
      rc = __vdbe_pma_reader_incr_merge_init(readr, mode);
    }
  }
  return rc;
}

/*
** Allocate a new merge_engine_t object to merge the contents of n_pma level-0
** PMAs from task->file. If no error occurs, set *out to point to
** the new object and return DBSQL_OK. Or, if an error does occur, set *out
** to NULL and return an dbsql error code.
**
** When this function is called, *pi_offset is set to the offset of the
** first PMA to read from task->file. Assuming no error occurs, it is
** set to the offset immediately following the last byte of the last
** PMA before returning. If an error does occur, then the final value of
** *pi_offset is undefined.
*/
static int __vdbe_merge_engine_level0(
  sort_subtask_t *task,             /* Sorter task to read from */
  int n_pma,                       /* Number of PMAs to read */
  int64_t *pi_offset,                  /* IN/OUT: Readr offset in task->file */
  merge_engine_t **out             /* OUT: New merge-engine */
){
  merge_engine_t *p_new;              /* Merge engine to return */
  int64_t i_off = *pi_offset;
  int i;
  int rc = DBSQL_OK;

  *out = p_new = __vdbe_merge_engine_new(n_pma);
  if( p_new==0 ) rc = DBSQL_NOMEM_BKPT;

  for(i=0; i<n_pma && rc==DBSQL_OK; i++){
    int64_t n_dummy = 0;
    pma_reader_t *readr = &p_new->readrs[i];
    rc = __vdbe_pma_reader_init(task, &task->file, i_off, readr, &n_dummy);
    i_off = readr->i_eof;
  }

  if( rc!=DBSQL_OK ){
    __vdbe_merge_engine_free(p_new);
    *out = 0;
  }
  *pi_offset = i_off;
  return rc;
}

/*
** Return the depth of a tree comprising n_pma PMAs, assuming a fanout of
** SORTER_MAX_MERGE_COUNT. The returned value does not include leaf nodes.
**
** i.e.
**
**   n_pma<=16    -> __tree_depth() == 0
**   n_pma<=256   -> __tree_depth() == 1
**   n_pma<=65536 -> __tree_depth() == 2
*/
static int __vdbe_sorter_tree_depth(int n_pma){
  int n_depth = 0;
  int64_t n_div = SORTER_MAX_MERGE_COUNT;
  while( n_div < (int64_t)n_pma ){
    n_div = n_div * SORTER_MAX_MERGE_COUNT;
    n_depth++;
  }
  return n_depth;
}

/*
** root is the root of an incremental merge-tree with depth n_depth (according
** to __vdbe_sorter_tree_depth()). leaf is the i_seq'th leaf to be added to the
** tree, counting from zero. This function adds leaf to the tree.
**
** If successful, DBSQL_OK is returned. If an error occurs, an dbsql error
** code is returned and leaf is freed.
*/
static int __vdbe_sorter_add_to_tree(
  sort_subtask_t *task,             /* Task context */
  int n_depth,                     /* Depth of tree according to __tree_depth() */
  int i_seq,                       /* Sequence number of leaf within tree */
  merge_engine_t *root,             /* Root of tree */
  merge_engine_t *leaf              /* Leaf to add to tree */
){
  int rc = DBSQL_OK;
  int n_div = 1;
  int i;
  merge_engine_t *p = root;
  incr_merger_t *incr;

  rc = __vdbe_incr_merger_new(task, leaf, &incr);

  for(i=1; i<n_depth; i++){
    n_div = n_div * SORTER_MAX_MERGE_COUNT;
  }

  for(i=1; i<n_depth && rc==DBSQL_OK; i++){
    int i_iter = (i_seq / n_div) % SORTER_MAX_MERGE_COUNT;
    pma_reader_t *readr = &p->readrs[i_iter];

    if( readr->incr==0 ){
      merge_engine_t *p_new = __vdbe_merge_engine_new(SORTER_MAX_MERGE_COUNT);
      if( p_new==0 ){
        rc = DBSQL_NOMEM_BKPT;
      }else{
        rc = __vdbe_incr_merger_new(task, p_new, &readr->incr);
      }
    }
    if( rc==DBSQL_OK ){
      p = readr->incr->merger;
      n_div = n_div / SORTER_MAX_MERGE_COUNT;
    }
  }

  if( rc==DBSQL_OK ){
    p->readrs[i_seq % SORTER_MAX_MERGE_COUNT].incr = incr;
  }else{
    __vdbe_incr_free(incr);
  }
  return rc;
}

/*
** This function is called as part of a __sorter_rewind() operation on a sorter
** that has already written two or more level-0 PMAs to one or more temp
** files. It builds a tree of merge_engine_t/incr_merger_t/pma_reader_t objects that
** can be used to incrementally merge all PMAs on disk.
**
** If successful, DBSQL_OK is returned and *out set to point to the
** merge_engine_t object at the root of the tree before returning. Or, if an
** error occurs, an dbsql error code is returned and the final value
** of *out is undefined.
*/
static int __vdbe_sorter_merge_tree_build(
  vdbe_sorter_t *sorter,       /* The VDBE cursor that implements the sort */
  merge_engine_t **out        /* Write the merge_engine_t here */
){
  merge_engine_t *p_main = 0;
  int rc = DBSQL_OK;
  int i_task;

#if DBSQL_MAX_WORKER_THREADS>0
  /* If the sorter uses more than one task, then create the top-level
  ** merge_engine_t here. This merge_engine_t will read data from exactly
  ** one pma_reader_t per sub-task.  */
  assert( sorter->use_threads || sorter->n_task==1 );
  if( sorter->n_task>1 ){
    p_main = __vdbe_merge_engine_new(sorter->n_task);
    if( p_main==0 ) rc = DBSQL_NOMEM_BKPT;
  }
#endif

  for(i_task=0; rc==DBSQL_OK && i_task<sorter->n_task; i_task++){
    sort_subtask_t *task = &sorter->tasks[i_task];
    assert( task->n_pma>0 || DBSQL_MAX_WORKER_THREADS>0 );
    if( DBSQL_MAX_WORKER_THREADS==0 || task->n_pma ){
      merge_engine_t *root = 0;     /* Root node of tree for this task */
      int n_depth = __vdbe_sorter_tree_depth(task->n_pma);
      int64_t i_read_off = 0;

      if( task->n_pma<=SORTER_MAX_MERGE_COUNT ){
        rc = __vdbe_merge_engine_level0(task, task->n_pma, &i_read_off, &root);
      }else{
        int i;
        int i_seq = 0;
        root = __vdbe_merge_engine_new(SORTER_MAX_MERGE_COUNT);
        if( root==0 ) rc = DBSQL_NOMEM_BKPT;
        for(i=0; i<task->n_pma && rc==DBSQL_OK; i += SORTER_MAX_MERGE_COUNT){
          merge_engine_t *merger = 0; /* New level-0 PMA merger */
          int n_reader;              /* Number of level-0 PMAs to merge */

          n_reader = MIN(task->n_pma - i, SORTER_MAX_MERGE_COUNT);
          rc = __vdbe_merge_engine_level0(task, n_reader, &i_read_off, &merger);
          if( rc==DBSQL_OK ){
            rc = __vdbe_sorter_add_to_tree(task, n_depth, i_seq++, root, merger);
          }
        }
      }

      if( rc==DBSQL_OK ){
#if DBSQL_MAX_WORKER_THREADS>0
        if( p_main!=0 ){
          rc = __vdbe_incr_merger_new(task, root, &p_main->readrs[i_task].incr);
        }else
#endif
        {
          assert( p_main==0 );
          p_main = root;
        }
      }else{
        __vdbe_merge_engine_free(root);
      }
    }
  }

  if( rc!=DBSQL_OK ){
    __vdbe_merge_engine_free(p_main);
    p_main = 0;
  }
  *out = p_main;
  return rc;
}

/*
** This function is called as part of an __vdbe_sorter_rewind() operation
** on a sorter that has written two or more PMAs to temporary files. It sets
** up either vdbe_sorter_t.merger (for single threaded sorters) or reader
** (for multi-threaded sorters) so that it can be used to iterate through
** all records stored in the sorter.
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
*/
static int __vdbe_sorter_setup_merge(vdbe_sorter_t *sorter){
  int rc;                         /* Return code */
  sort_subtask_t *task0 = &sorter->tasks[0];
  merge_engine_t *p_main = 0;
#if DBSQL_MAX_WORKER_THREADS
  DBSQL *db = task0->sorter->db;
  int i;
  __sorter_compare compare = __vdbe_sorter_get_compare(sorter);
  for(i=0; i<sorter->n_task; i++){
    sorter->tasks[i].compare = compare;
  }
#endif

  rc = __vdbe_sorter_merge_tree_build(sorter, &p_main);
  if( rc==DBSQL_OK ){
#if DBSQL_MAX_WORKER_THREADS
    assert( sorter->use_threads==0 || sorter->n_task>1 );
    if( sorter->use_threads ){
      int i_task;
      pma_reader_t *readr = 0;
      sort_subtask_t *last = &sorter->tasks[sorter->n_task-1];
      rc = __vdbe_sort_alloc_unpacked(last);
      if( rc==DBSQL_OK ){
        readr = (pma_reader_t*)__db_malloc_zero(db, sizeof(pma_reader_t));
        sorter->reader = readr;
        if( readr==0 ) rc = DBSQL_NOMEM_BKPT;
      }
      if( rc==DBSQL_OK ){
        rc = __vdbe_incr_merger_new(last, p_main, &readr->incr);
        if( rc==DBSQL_OK ){
          __vdbe_incr_merger_set_threads(readr->incr);
          for(i_task=0; i_task<(sorter->n_task-1); i_task++){
            incr_merger_t *incr;
            if( (incr = p_main->readrs[i_task].incr) ){
              __vdbe_incr_merger_set_threads(incr);
              assert( incr->task!=last );
            }
          }
          for(i_task=0; rc==DBSQL_OK && i_task<sorter->n_task; i_task++){
            /* Check that:
            **  
            **   a) The incremental merge object is configured to use the
            **      right task, and
            **   b) If it is using task (n_task-1), it is configured to run
            **      in single-threaded mode. This is important, as the
            **      root merge (INCRINIT_ROOT) will be using the same task
            **      object.
            */
            pma_reader_t *p = &p_main->readrs[i_task];
            assert( p->incr==0 || (
                (p->incr->task==&sorter->tasks[i_task])             /* a */
             && (i_task!=sorter->n_task-1 || p->incr->use_thread==0)  /* b */
            ));
            rc = __vdbe_pma_reader_incr_init(p, INCRINIT_TASK);
          }
        }
        p_main = 0;
      }
      if( rc==DBSQL_OK ){
        rc = __vdbe_pma_reader_incr_merge_init(readr, INCRINIT_ROOT);
      }
    }else
#endif
    {
      rc = __vdbe_merge_engine_init(task0, p_main, INCRINIT_NORMAL);
      sorter->merger = p_main;
      p_main = 0;
    }
  }

  if( rc!=DBSQL_OK ){
    __vdbe_merge_engine_free(p_main);
  }
  return rc;
}


/*
** Once the sorter has been populated by calls to __vdbe_sorter_write,
** this function is called to prepare for iterating through the records
** in sorted order.
*/
int __vdbe_sorter_rewind(const vdbe_cursor_t *csr, int *pb_eof){
  vdbe_sorter_t *sorter;
  int rc = DBSQL_OK;             /* Return code */

  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  assert( sorter );

  /* If no data has been written to disk, then do not do so now. Instead,
  ** sort the vdbe_sorter_t.record list. The vdbe layer will read data directly
  ** from the in-memory list.  */
  if( sorter->use_pma==0 ){
    if( sorter->list.list ){
      *pb_eof = 0;
      rc = __vdbe_sorter_sort(&sorter->tasks[0], &sorter->list);
    }else{
      *pb_eof = 1;
    }
    return rc;
  }

  /* Write the current in-memory list to a PMA. When the __vdbe_sorter_write()
  ** function flushes the contents of memory to disk, it immediately always
  ** creates a new list consisting of a single key immediately afterwards.
  ** So the list is never empty at this point.  */
  assert( sorter->list.list );
  rc = __vdbe_sorter_flush_pma(sorter);

  /* Join all threads */
  rc = VDBE_SORTER_JOIN_ALL(sorter, rc);

  VDBE_SORTER_REWIND_DEBUG("rewind");

  /* Assuming no errors have occurred, set up a merger structure to
  ** incrementally read and merge all remaining PMAs.  */
  assert( sorter->reader==0 );
  if( rc==DBSQL_OK ){
    rc = __vdbe_sorter_setup_merge(sorter);
    *pb_eof = 0;
  }

  VDBE_SORTER_REWIND_DEBUG("rewinddone");
  return rc;
}

/*
** Advance to the next element in the sorter.  Return value:
**
**    DBSQL_OK     success
**    DBSQL_DONE   end of data
**    otherwise     some kind of error.
*/
int __vdbe_sorter_next(DBSQL *db, const vdbe_cursor_t *csr){
  vdbe_sorter_t *sorter;
  int rc;                         /* Return code */

  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  assert( sorter->use_pma || (sorter->reader==0 && sorter->merger==0) );
  if( sorter->use_pma ){
    assert( sorter->reader==0 || sorter->merger==0 );
    assert( sorter->use_threads==0 || sorter->reader );
    assert( sorter->use_threads==1 || sorter->merger );
#if DBSQL_MAX_WORKER_THREADS>0
    if( sorter->use_threads ){
      rc = __vdbe_pma_reader_next(sorter->reader);
      if( rc==DBSQL_OK && sorter->reader->fd==0 ) rc = DBSQL_DONE;
    }else
#endif
    /*if( !sorter->use_threads )*/ {
      int res = 0;
      assert( sorter->merger!=0 );
      assert( sorter->merger->task==(&sorter->tasks[0]) );
      rc = __vdbe_merge_engine_step(sorter->merger, &res);
      if( rc==DBSQL_OK && res ) rc = DBSQL_DONE;
    }
  }else{
    sorter_record_t *p_free = sorter->list.list;
    sorter->list.list = p_free->u.next;
    p_free->u.next = 0;
    if( sorter->list.memories==0 ) __vdbe_sorter_record_free(db, p_free);
    rc = sorter->list.list ? DBSQL_OK : DBSQL_DONE;
  }
  return rc;
}

/*
** Return a pointer to a buffer owned by the sorter that contains the
** current key.
*/
static void *__vdbe_sorter_rowkey_local(
  const vdbe_sorter_t *sorter,      /* Sorter object */
  int *pn_key                      /* OUT: Size of current key in bytes */
){
  void *key;
  if( sorter->use_pma ){
    pma_reader_t *reader;
#if DBSQL_MAX_WORKER_THREADS>0
    if( sorter->use_threads ){
      reader = sorter->reader;
    }else
#endif
    /*if( !sorter->use_threads )*/{
      reader = &sorter->merger->readrs[sorter->merger->trees[1]];
    }
    *pn_key = reader->n_key;
    key = reader->keys;
  }else{
    *pn_key = sorter->list.list->n_val;
    key = SRVAL(sorter->list.list);
  }
  return key;
}

/*
** Copy the current sorter key into the memory cell out.
*/
int __vdbe_sorter_rowkey(const vdbe_cursor_t *csr, mem_t *out){
  vdbe_sorter_t *sorter;
  void *key; int n_key;           /* Sorter key to copy into out */

  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  key = __vdbe_sorter_rowkey_local(sorter, &n_key);
  if( __vdbe_mem_clear_and_resize(out, n_key) ){
    return DBSQL_NOMEM_BKPT;
  }
  out->n = n_key;
  MEM_SET_TYPE_FLAG(out, MEM_Blob);
  memcpy(out->z, key, n_key);

  return DBSQL_OK;
}

/*
** Compare the key in memory cell val with the key that the sorter cursor
** passed as the first argument currently points to. For the purposes of
** the comparison, ignore the rowid field at the end of each record.
**
** If the sorter cursor key contains any NULL values, consider it to be
** less than val. Even if val also contains NULL values.
**
** If an error occurs, return an dbsql error code (i.e. DBSQL_NOMEM).
** Otherwise, set *res to a negative, zero or positive value if the
** key in val is smaller than, equal to or larger than the current sorter
** key.
**
** This routine forms the core of the OP_SorterCompare opcode, which in
** turn is used to verify uniqueness when constructing a UNIQUE INDEX.
*/
int __vdbe_sorter_compare(
  const vdbe_cursor_t *csr,         /* Sorter cursor */
  mem_t *val,                      /* Value to compare to current sorter key */
  int n_key_col,                    /* Compare this many columns */
  int *res                       /* OUT: Result of comparison */
){
  vdbe_sorter_t *sorter;
  unpacked_record_t *r2;
  key_info_t *key_info;
  int i;
  void *key; int n_key;           /* Sorter key to compare val with */

  assert( csr->cur_type==CURTYPE_SORTER );
  sorter = csr->uc.sorter;
  r2 = sorter->unpacked;
  key_info = csr->key_info;
  if( r2==0 ){
    r2 = sorter->unpacked = __vdbe_alloc_unpacked_record(key_info);
    if( r2==0 ) return DBSQL_NOMEM_BKPT;
    r2->n_field = n_key_col;
  }
  assert( r2->n_field==n_key_col );

  key = __vdbe_sorter_rowkey_local(sorter, &n_key);
  __vdbe_record_unpack(n_key, key, r2);
  for(i=0; i<n_key_col; i++){
    if( r2->mems[i].flags & MEM_Null ){
      *res = -1;
      return DBSQL_OK;
    }
  }

  *res = __vdbe_record_compare(val->n, val->z, r2);
  return DBSQL_OK;
}
