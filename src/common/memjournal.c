/*
** 2008 October 7
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
** This file contains code use to implement an in-memory rollback journal.
** The in-memory rollback journal is used to journal transactions for
** ":memory:" databases and when the journal_mode=MEMORY pragma is used.
**
** Update:  The in-memory journal is also used to temporarily cache
** smaller journals that are not critical for power-loss recovery.
** For example, statement journals that are not too big will be held
** entirely in memory, thus reducing the number of file I/O calls, and
** more importantly, reducing temporary file creation events.  If these
** journals become too large for memory, they are spilled to disk.  But
** in the common case, they are usually small and no file I/O needs to
** occur.
*/
#include "dbsql_int.h"

/* Forward references to internal structures */
typedef struct mem_journal_t mem_journal_t;
typedef struct file_point_t file_point_t;
typedef struct file_chunk_t file_chunk_t;

/*
** The rollback journal is composed of a linked list of these structures.
**
** The chunk array is always at least 8 bytes in size - usually much more.
** Its actual size is stored in the mem_journal_t.n_chunk_size variable.
*/
struct file_chunk_t {
  file_chunk_t *next;               /* Next chunk in the journal */
  u_int8_t chunk[8];                   /* Content of this chunk */
};

/*
** By default, allocate this many bytes of memory for each file_chunk_t object.
*/
#define MEMJOURNAL_DFLT_FILECHUNKSIZE 1024

/*
** For chunk size n_chunk_size, return the number of bytes that should
** be allocated for each file_chunk_t structure.
*/
#define FILE_CHUNK_SIZE(n_chunk_size) (sizeof(file_chunk_t) + ((n_chunk_size)-8))

/*
** An instance of this object serves as a cursor into the rollback journal.
** The cursor can be either for reading or writing.
*/
struct file_point_t {
  int64_t i_offset;          /* Offset from the beginning of the file */
  file_chunk_t *chunk;              /* Specific chunk into which cursor points */
};

/*
** This structure is a subclass of dbsql_file_t. Each open memory-journal
** is an instance of this class.
*/
struct mem_journal_t {
  const dbsql_io_methods_t *method; /* Parent class. MUST BE FIRST */
  int n_chunk_size;                 /* In-memory chunk-size */

  int n_spill;                     /* Bytes of data before flushing */
  file_chunk_t *first;              /* Head of in-memory chunk-list */
  file_point_t endpoint;             /* Pointer to the end of the file */
  file_point_t readpoint;            /* Pointer to the end of the last x_read() */

  int flags;                      /* x_open flags */
  dbsql_vfs_t *vfs;              /* The "real" underlying VFS */
  const char *journal;           /* Name of the journal file */
};

/*
** Read data from the in-memory journal file.  This is the implementation
** of the dbsql_vfs_t.x_read method.
*/
static int __memjrnl_read(
  dbsql_file_t *jfd,    /* The journal file from which to read */
  void *buf,            /* Put the results here */
  int i_amt,              /* Number of bytes to read */
  int64_t i_ofst     /* Begin reading at this offset */
){
  mem_journal_t *p = (mem_journal_t *)jfd;
  u_int8_t *out = buf;
  int n_read = i_amt;
  int i_chunk_offset;
  file_chunk_t *chunk;

  if( (i_amt+i_ofst)>p->endpoint.i_offset ){
    return DBSQL_IOERR_SHORT_READ;
  }
  assert( p->readpoint.i_offset==0 || p->readpoint.chunk!=0 );
  if( p->readpoint.i_offset!=i_ofst || i_ofst==0 ){
    int64_t i_off = 0;
    for(chunk=p->first; 
        ALWAYS(chunk) && (i_off+p->n_chunk_size)<=i_ofst;
        chunk=chunk->next
    ){
      i_off += p->n_chunk_size;
    }
  }else{
    chunk = p->readpoint.chunk;
    assert( chunk!=0 );
  }

  i_chunk_offset = (int)(i_ofst%p->n_chunk_size);
  do {
    int i_space = p->n_chunk_size - i_chunk_offset;
    int n_copy = MIN(n_read, (p->n_chunk_size - i_chunk_offset));
    memcpy(out, (u_int8_t*)chunk->chunk + i_chunk_offset, n_copy);
    out += n_copy;
    n_read -= i_space;
    i_chunk_offset = 0;
  } while( n_read>=0 && (chunk=chunk->next)!=0 && n_read>0 );
  p->readpoint.i_offset = chunk ? i_ofst+i_amt : 0;
  p->readpoint.chunk = chunk;

  return DBSQL_OK;
}

/*
** Free the list of file_chunk_t structures headed at mem_journal_t.first.
*/
static void __memjrnl_free_chunks(file_chunk_t *first){
  file_chunk_t *iter;
  file_chunk_t *next;
  for(iter=first; iter; iter=next){
    next = iter->next;
    dbsql_free(iter);
  } 
}

/*
** Flush the contents of memory to a real file on disk.
*/
static int __memjrnl_create_file(mem_journal_t *p){
  int rc;
  dbsql_file_t *real = (dbsql_file_t*)p;
  mem_journal_t copy = *p;

  memset(p, 0, sizeof(mem_journal_t));
  rc = __dbsql_os_open(copy.vfs, copy.journal, real, copy.flags, 0);
  if( rc==DBSQL_OK ){
    int n_chunk = copy.n_chunk_size;
    int64_t i_off = 0;
    file_chunk_t *iter;
    for(iter=copy.first; iter; iter=iter->next){
      if( i_off + n_chunk > copy.endpoint.i_offset ){
        n_chunk = copy.endpoint.i_offset - i_off;
      }
      rc = __dbsql_os_write(real, (u_int8_t*)iter->chunk, n_chunk, i_off);
      if( rc ) break;
      i_off += n_chunk;
    }
    if( rc==DBSQL_OK ){
      /* No error has occurred. Free the in-memory buffers. */
      __memjrnl_free_chunks(copy.first);
    }
  }
  if( rc!=DBSQL_OK ){
    /* If an error occurred while creating or writing to the file, restore
    ** the original before returning. This way, dbsql uses the in-memory
    ** journal data to roll back changes made to the internal page-cache
    ** before this function was called.  */
    __dbsql_os_close(real);
    *p = copy;
  }
  return rc;
}


/* Forward reference */
static int __memjrnl_truncate(dbsql_file_t *jfd, int64_t size);

/*
** Write data to the file.
*/
static int __memjrnl_write(
  dbsql_file_t *jfd,    /* The journal file into which to write */
  const void *buf,      /* Take data to be written from here */
  int i_amt,              /* Number of bytes to write */
  int64_t i_ofst     /* Begin writing at this offset into the file */
){
  mem_journal_t *p = (mem_journal_t *)jfd;
  int n_write = i_amt;
  u_int8_t *z_write = (u_int8_t *)buf;

  /* If the file should be created now, create it and write the new data
  ** into the file on disk. */
  if( p->n_spill>0 && (i_amt+i_ofst)>p->n_spill ){
    int rc = __memjrnl_create_file(p);
    if( rc==DBSQL_OK ){
      rc = __dbsql_os_write(jfd, buf, i_amt, i_ofst);
    }
    return rc;
  }

  /* If the contents of this write should be stored in memory */
  else{
    /* An in-memory journal file should only ever be appended to. Random
    ** access writes are not required. The only exception to this is when
    ** the in-memory journal is being used by a connection using the
    ** atomic-write optimization. In this case the first 28 bytes of the
    ** journal file may be written as part of committing the transaction. */
    assert( i_ofst<=p->endpoint.i_offset );
    if( i_ofst>0 && i_ofst!=p->endpoint.i_offset ){
      __memjrnl_truncate(jfd, i_ofst);
    }
    if( i_ofst==0 && p->first ){
      assert( p->n_chunk_size>i_amt );
      memcpy((u_int8_t*)p->first->chunk, buf, i_amt);
    }else{
      while( n_write>0 ){
        file_chunk_t *chunk = p->endpoint.chunk;
        int i_chunk_offset = (int)(p->endpoint.i_offset%p->n_chunk_size);
        int i_space = MIN(n_write, p->n_chunk_size - i_chunk_offset);

        assert( chunk!=0 || i_chunk_offset==0 );
        if( i_chunk_offset==0 ){
          /* New chunk is required to extend the file. */
          file_chunk_t *p_new = dbsql_malloc(FILE_CHUNK_SIZE(p->n_chunk_size));
          if( !p_new ){
            return DBSQL_IOERR_NOMEM_BKPT;
          }
          p_new->next = 0;
          if( chunk ){
            assert( p->first );
            chunk->next = p_new;
          }else{
            assert( !p->first );
            p->first = p_new;
          }
          chunk = p->endpoint.chunk = p_new;
        }

        assert( chunk!=0 );
        memcpy((u_int8_t*)chunk->chunk + i_chunk_offset, z_write, i_space);
        z_write += i_space;
        n_write -= i_space;
        p->endpoint.i_offset += i_space;
      }
    }
  }

  return DBSQL_OK;
}

/*
** Truncate the in-memory file.
*/
static int __memjrnl_truncate(dbsql_file_t *jfd, int64_t size){
  mem_journal_t *p = (mem_journal_t *)jfd;
  assert( p->endpoint.chunk==0 || p->endpoint.chunk->next==0 );
  if( size<p->endpoint.i_offset ){
    file_chunk_t *iter = 0;
    if( size==0 ){
      __memjrnl_free_chunks(p->first);
      p->first = 0;
    }else{
      int64_t i_off = p->n_chunk_size;
      for(iter=p->first; ALWAYS(iter) && i_off<size; iter=iter->next){
        i_off += p->n_chunk_size;
      }
      if( ALWAYS(iter) ){
        __memjrnl_free_chunks(iter->next);
        iter->next = 0;
      }
    }

    p->endpoint.chunk = iter;
    p->endpoint.i_offset = size;
    p->readpoint.chunk = 0;
    p->readpoint.i_offset = 0;
  }
  return DBSQL_OK;
}

/*
** Close the file.
*/
static int __memjrnl_close(dbsql_file_t *jfd){
  mem_journal_t *p = (mem_journal_t *)jfd;
  __memjrnl_free_chunks(p->first);
  return DBSQL_OK;
}

/*
** Sync the file.
**
** If the real file has been created, call its x_sync method. Otherwise, 
** syncing an in-memory journal is a no-op. 
*/
static int __memjrnl_sync(dbsql_file_t *jfd, int flags){
  UNUSED_PARAMETER2(jfd, flags);
  return DBSQL_OK;
}

/*
** Query the size of the file in bytes.
*/
static int __memjrnl_file_size(dbsql_file_t *jfd, int64_t *size){
  mem_journal_t *p = (mem_journal_t *)jfd;
  *size = (int64_t) p->endpoint.i_offset;
  return DBSQL_OK;
}

/*
** table_t of methods for mem_journal_t dbsql_file_t object.
*/
static const struct dbsql_io_methods_t __mem_journal_methods = {
  1,                /* i_version */
  __memjrnl_close,     /* x_close */
  __memjrnl_read,      /* x_read */
  __memjrnl_write,     /* x_write */
  __memjrnl_truncate,  /* truncate */
  __memjrnl_sync,      /* x_sync */
  __memjrnl_file_size,  /* file_size */
  0,                /* lock */
  0,                /* unlock */
  0,                /* check_reserved_lock */
  0,                /* file_control */
  0,                /* sector_size */
  0,                /* device_characteristics */
  0,                /* shm_map */
  0,                /* shm_lock */
  0,                /* shm_barrier */
  0,                /* shm_unmap */
  0,                /* fetch */
  0                 /* unfetch */
};

/* 
** Open a journal file. 
**
** The behaviour of the journal file depends on the value of parameter 
** n_spill. If n_spill is 0, then the journal file is always create and 
** accessed using the underlying VFS. If n_spill is less than zero, then
** all content is always stored in main-memory. Finally, if n_spill is a
** positive value, then the journal file is initially created in-memory
** but may be flushed to disk later on. In this case the journal file is
** flushed to disk either when it grows larger than n_spill bytes in size,
** or when __journal_create() is called.
*/
int __journal_open(
  dbsql_vfs_t *vfs,         /* The VFS to use for actual file I/O */
  const char *name,         /* Name of the journal file */
  dbsql_file_t *jfd,        /* Preallocated, blank file handle */
  int flags,                 /* Opening flags */
  int n_spill                 /* Bytes buffered before opening the file */
){
  mem_journal_t *p = (mem_journal_t*)jfd;

  assert( name || n_spill<0 || (flags & DBSQL_OPEN_EXCLUSIVE) );

  /* Zero the file-handle object. If n_spill was passed zero, initialize
  ** it using the __dbsql_os_open() function of the underlying VFS. In this
  ** case none of the code in this module is executed as a result of calls
  ** made on the journal file-handle.  */
  memset(p, 0, sizeof(mem_journal_t));
  if( n_spill==0 ){
    return __dbsql_os_open(vfs, name, jfd, flags, 0);
  }

  if( n_spill>0 ){
    p->n_chunk_size = n_spill;
  }else{
    p->n_chunk_size = 8 + MEMJOURNAL_DFLT_FILECHUNKSIZE - sizeof(file_chunk_t);
    assert( MEMJOURNAL_DFLT_FILECHUNKSIZE==FILE_CHUNK_SIZE(p->n_chunk_size) );
  }

  jfd->methods = (const dbsql_io_methods_t*)&__mem_journal_methods;
  p->n_spill = n_spill;
  p->flags = flags;
  p->journal = name;
  p->vfs = vfs;
  return DBSQL_OK;
}

/*
** Open an in-memory journal file.
*/
void __mem_journal_open(dbsql_file_t *jfd){
  __journal_open(0, 0, jfd, 0, -1);
}

#if defined(DBSQL_ENABLE_ATOMIC_WRITE) \
 || defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
/*
** If the argument p points to a mem_journal_t structure that is not an 
** in-memory-only journal file (i.e. is one that was opened with a +ve
** n_spill parameter or as DBSQL_OPEN_MAIN_JOURNAL), and the underlying 
** file has not yet been created, create it now.
*/
int __journal_create(dbsql_file_t *jfd){
  int rc = DBSQL_OK;
  mem_journal_t *p = (mem_journal_t*)jfd;
  if( jfd->methods==&__mem_journal_methods && (
#ifdef DBSQL_ENABLE_ATOMIC_WRITE
     p->n_spill>0
#else
     /* While this appears to not be possible without ATOMIC_WRITE, the
     ** paths are complex, so it seems prudent to leave the test in as
     ** a NEVER(), in case our analysis is subtly flawed. */
     NEVER(p->n_spill>0)
#endif
#ifdef DBSQL_ENABLE_BATCH_ATOMIC_WRITE
     || (p->flags & DBSQL_OPEN_MAIN_JOURNAL)
#endif
  )){
    rc = __memjrnl_create_file(p);
  }
  return rc;
}
#endif

/*
** The file-handle passed as the only argument is open on a journal file.
** Return true if this "journal file" is currently stored in heap memory,
** or false otherwise.
*/
int __journal_is_in_memory(dbsql_file_t *p){
  return p->methods==&__mem_journal_methods;
}

/* 
** Return the number of bytes required to store a __journal_file that uses vfs
** vfs to create the underlying on-disk files.
*/
int __journal_size(dbsql_vfs_t *vfs){
  return MAX(vfs->sz_os_file, (int)sizeof(mem_journal_t));
}
