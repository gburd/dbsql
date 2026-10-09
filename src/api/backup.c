/*
** 2009 January 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of the dbsql_backup_XXX() 
** API functions and the related features.
*/
#include "dbsql_int.h"
#include "btree_int.h"

/*
** Structure allocated for each backup operation.
*/
struct dbsql_backup_t {
  DBSQL* dest_db;        /* Destination database handle */
  char *z_dest_db;
  sm_t *dest;            /* Destination b-tree file */
  u_int32_t i_dest_schema;         /* Original schema cookie in destination */
  int dest_locked;         /* True once a write-transaction is open on dest */

  pgno_t i_next;              /* Page number of the next source page to copy */
  DBSQL* src_db;         /* Source database handle */
  sm_t *src;             /* Source b-tree file */

  int rc;                  /* Backup process error code */

  /* These two variables are set by every call to backup_step(). They are
  ** read by calls to backup_remaining() and backup_pagecount().
  */
  pgno_t n_remaining;         /* Number of pages left to copy */
  pgno_t n_pagecount;         /* Total number of pages to copy */

  int is_attached;          /* True once backup has been registered with pager */
  dbsql_backup_t *next;   /* Next backup associated with source pager */
};

/*
** THREAD SAFETY NOTES:
**
**   Once it has been created using backup_init(), a single dbsql_backup_t
**   structure may be accessed via two groups of thread-safe entry points:
**
**     * Via the dbsql_backup_XXX() API function backup_step() and 
**       backup_finish(). Both these functions obtain the source database
**       handle mutex and the mutex associated with the source sm_shared_t 
**       structure, in that order.
**
**     * Via the __backup_update() and __backup_restart() functions, which are
**       invoked by the pager layer to report various state changes in
**       the page cache associated with the source database. The mutex
**       associated with the source database sm_shared_t structure will always 
**       be held when either of these functions are invoked.
**
**   The other dbsql_backup_XXX() API functions, backup_remaining() and
**   backup_pagecount() are not thread-safe functions. If they are called
**   while some other thread is calling backup_step() or backup_finish(),
**   the values returned may be invalid. There is no way for a call to
**   __backup_update() or __backup_restart() to interfere with backup_remaining()
**   or backup_pagecount().
**
**   Depending on the dbsql configuration, the database handles and/or
**   the sm_t objects may have their own mutexes that require locking.
**   Non-sharable Btrees (in-memory databases for example), do not have
**   associated mutexes.
*/

/*
** Return a pointer corresponding to database db (i.e. "main", "temp")
** in connection handle db. If such a database cannot be found, return
** a NULL pointer and write an error message to error_db.
**
** If the "temp" database is requested, it may need to be opened by this 
** function. If an error occurs while doing so, return 0 and write an 
** error message to error_db.
*/
static sm_t *__find_btree(DBSQL *error_db, DBSQL *db, const char *z_db){
  int i = __find_db_name(db, z_db);

  if( i==1 ){
    parser_t s_parse;
    int rc = 0;
    __parse_object_init(&s_parse,db);
    if( __open_temp_database(&s_parse) ){
      __error_with_msg(error_db, s_parse.rc, "%s", s_parse.errmsg);
      rc = DBSQL_ERROR;
    }
    __dbsql_db_free(error_db, s_parse.errmsg);
    __parse_object_reset(&s_parse);
    if( rc ){
      return 0;
    }
  }

  if( i<0 ){
    __error_with_msg(error_db, DBSQL_ERROR, "unknown database %s", z_db);
    return 0;
  }

  return db->dbs[i].bt;
}

/*
** Attempt to set the page size of the destination to match the page size
** of the source.
*/
static int __set_dest_pgsz(sm_t *dest, sm_t *src){
  return __sm_set_page_size(dest, __sm_get_page_size(src), 0, 0);
}

/*
** Check that there is no open read-transaction on the b-tree passed as the
** second argument. If there is not, return DBSQL_OK. Otherwise, if there
** is an open read-transaction, return DBSQL_ERROR and leave an error 
** message in database handle db.
*/
static int __check_read_transaction(DBSQL *db, sm_t *p){
  if( __sm_txn_state(p)!=DBSQL_TXN_NONE ){
    __error_with_msg(db, DBSQL_ERROR, "destination database is in use");
    return DBSQL_ERROR;
  }
  return DBSQL_OK;
}

/*
** Create an dbsql_backup_t process to copy the contents of src_db from
** connection handle src_db to dest_db in dest_db. If successful, return
** a pointer to the new dbsql_backup_t object.
**
** If an error occurs, NULL is returned and an error code and error message
** stored in database handle dest_db.
*/
dbsql_backup_t *dbsql_backup_init(
  DBSQL* dest_db,                     /* Database to write to */
  const char *z_dest_db,                  /* Name of database within dest_db */
  DBSQL* src_db,                      /* Database connection to read from */
  const char *z_src_db                    /* Name of database within src_db */
){
  dbsql_backup_t *p;                    /* Value to return */

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(src_db)||!__safety_check_ok(dest_db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif

  /* Lock the source database handle. The destination database
  ** handle is not locked in this routine, but it is locked in
  ** dbsql_backup_step(). The user is required to ensure that no
  ** other thread accesses the destination handle for the duration
  ** of the backup operation.  Any attempt to use the destination
  ** database connection while a backup is in progress may cause
  ** a malfunction or a deadlock.
  */
  dbsql_mutex_enter(src_db->mutex);
  dbsql_mutex_enter(dest_db->mutex);

  if( src_db==dest_db ){
    __error_with_msg(
        dest_db, DBSQL_ERROR, "source and destination must be distinct"
    );
    p = 0;
  }else {
    int n_dest = __strlen30(z_dest_db);

    /* Allocate space for a new dbsql_backup_t object...
    ** EVIDENCE-OF: R-64852-21591 The dbsql_backup_t object is created by a
    ** call to dbsql_backup_init() and is destroyed by a call to
    ** dbsql_backup_finish(). */
    p = (dbsql_backup_t*)__malloc_zero(sizeof(dbsql_backup_t)+n_dest+1);
    if( !p ){
      __dbsql_error(dest_db, DBSQL_NOMEM_BKPT);
    }else{
      p->z_dest_db = (char*)&p[1];
      memcpy(p->z_dest_db, z_dest_db, n_dest);
    }
  }

  /* If the allocation succeeded, populate the new object. */
  if( p ){
    /* Do not store the pointer to the destination b-tree at this point.
    ** This is because there is nothing preventing it from being detached
    ** or otherwise freed before the first call to dbsql_backup_step()
    ** on this object. The source b-tree does not have this problem, as
    ** incrementing sm_t.n_backup (see below) effectively locks the object. */
    sm_t *dest = __find_btree(dest_db, dest_db, z_dest_db);
    p->src = __find_btree(dest_db, src_db, z_src_db);
    p->dest_db = dest_db;
    p->src_db = src_db;
    p->i_next = 1;
    p->is_attached = 0;

    if( 0==p->src || 0==dest 
     || __check_read_transaction(dest_db, dest)!=DBSQL_OK 
     ){
      /* One (or both) of the named databases did not exist or an OOM
      ** error was hit. Or there is a transaction open on the destination
      ** database. The error has already been written into the dest_db 
      ** handle. All that is left to do here is free the dbsql_backup_t 
      ** structure.  */
      dbsql_free(p);
      p = 0;
    }
  }
  if( p ){
    p->src->n_backup++;
  }

  dbsql_mutex_leave(dest_db->mutex);
  dbsql_mutex_leave(src_db->mutex);
  return p;
}

/*
** Argument rc is an dbsql error code. Return true if this error is 
** considered fatal if encountered during a backup operation. All errors
** are considered fatal except for DBSQL_BUSY and DBSQL_LOCKED.
*/
static int __is_fatal_error(int rc){
  return (rc!=DBSQL_OK && rc!=DBSQL_BUSY && ALWAYS(rc!=DBSQL_LOCKED));
}

/*
** Parameter src_data points to a buffer containing the data for 
** page i_src_pg from the source database. Copy this data into the 
** destination database.
*/
static int __backup_one_page(
  dbsql_backup_t *p,              /* Backup handle */
  pgno_t i_src_pg,                    /* Source database page to backup */
  const u_int8_t *src_data,             /* Source database page data */
  int update                     /* True for an update, false otherwise */
){
  pager_t * const dest_pager = __sm_pager(p->dest);
  const int n_src_pgsz = __sm_get_page_size(p->src);
  int n_dest_pgsz = __sm_get_page_size(p->dest);
  const int n_copy = MIN(n_src_pgsz, n_dest_pgsz);
  const int64_t i_end = (int64_t)i_src_pg*(int64_t)n_src_pgsz;
  int rc = DBSQL_OK;
  int64_t i_off;

  assert( __sm_get_reserve_no_mutex(p->src)>=0 );
  assert( p->dest_locked );
  assert( !__is_fatal_error(p->rc) );
  assert( i_src_pg!=PENDING_BYTE_PAGE(p->src->bt) );
  assert( src_data );
  assert( n_src_pgsz==n_dest_pgsz || __sm_pager_is_memdb(dest_pager)==0 );

  /* This loop runs once for each destination page spanned by the source 
  ** page. For each iteration, variable i_off is set to the byte offset
  ** of the destination page.
  */
  for(i_off=i_end-(int64_t)n_src_pgsz; rc==DBSQL_OK && i_off<i_end; i_off+=n_dest_pgsz){
    db_page_t *dest_pg = 0;
    pgno_t i_dest = (pgno_t)(i_off/n_dest_pgsz)+1;
    if( i_dest==PENDING_BYTE_PAGE(p->dest->bt) ) continue;
    if( DBSQL_OK==(rc = __sm_pager_get(dest_pager, i_dest, &dest_pg, 0))
     && DBSQL_OK==(rc = __sm_pager_write(dest_pg))
    ){
      const u_int8_t *in = &src_data[i_off%n_src_pgsz];
      u_int8_t *dest_data = __sm_pager_get_data(dest_pg);
      u_int8_t *out = &dest_data[i_off%n_dest_pgsz];

      /* Copy the data from the source page into the destination page.
      ** Then clear the sm_t layer __mem_page.is_init flag. Both this module
      ** and the pager code use this trick (clearing the first byte
      ** of the page 'extra' space to invalidate the sm_t layers
      ** cached parse of the page). __mem_page.is_init is marked 
      ** "MUST BE FIRST" for this purpose.
      */
      memcpy(out, in, n_copy);
      ((u_int8_t *)__sm_pager_get_extra(dest_pg))[0] = 0;
      if( i_off==0 && update==0 ){
        __put4byte(&out[28], __sm_last_page(p->src));
      }
    }
    __sm_pager_unref(dest_pg);
  }

  return rc;
}

/*
** If file is currently larger than i_size bytes, then truncate it to
** exactly i_size bytes. If file is not larger than i_size bytes, then
** this function is a no-op.
**
** Return DBSQL_OK if everything is successful, or an dbsql error 
** code if an error occurs.
*/
static int __backup_truncate_file(dbsql_file_t *file, int64_t i_size){
  int64_t i_current;
  int rc = __dbsql_os_file_size(file, &i_current);
  if( rc==DBSQL_OK && i_current>i_size ){
    rc = __dbsql_os_truncate(file, i_size);
  }
  return rc;
}

/*
** Register this backup object with the associated source pager for
** callbacks when pages are changed or the cache invalidated.
*/
static void __attach_backup_object(dbsql_backup_t *p){
  dbsql_backup_t **pp;
  assert( __sm_holds_mutex(p->src) );
  pp = __sm_pager_backup_ptr(__sm_pager(p->src));
  p->next = *pp;
  *pp = p;
  p->is_attached = 1;
}

/*
** Copy n_page pages from the source b-tree to the destination.
*/
int dbsql_backup_step(dbsql_backup_t *p, int n_page){
  int rc;
  int dest_mode = 0;   /* Destination journal mode */
  int pgsz_src = 0;    /* Source page size */
  int pgsz_dest = 0;   /* Destination page size */

#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(p->src_db->mutex);
  __sm_enter(p->src);
  if( p->dest_db ){
    dbsql_mutex_enter(p->dest_db->mutex);
  }

  rc = p->rc;
  if( !__is_fatal_error(rc) ){
    pager_t * const src_pager = __sm_pager(p->src);     /* Source pager */
    sm_t * dest = 0;                                        /* Dest btree */
    pager_t * dest_pager = 0;                                   /* Dest pager */
    int ii;                            /* Iterator variable */
    int n_src_page = -1;                 /* Size of source db in pages */
    int close_trans = 0;               /* True if src db requires unlocking */

    /* If the source pager is currently in a write-transaction, return
    ** DBSQL_BUSY immediately.
    */
    if( p->dest_db && p->src->bt->in_transaction==TRANS_WRITE ){
      rc = DBSQL_BUSY;
    }else{
      rc = DBSQL_OK;
    }


    /* If there is no open read-transaction on the source database, open
    ** one now. If a transaction is opened here, then it will be closed
    ** before this function exits.
    */
    if( rc==DBSQL_OK && DBSQL_TXN_NONE==__sm_txn_state(p->src) ){
      rc = __sm_begin_trans(p->src, 0, 0);
      close_trans = 1;
    }

    /* Locate the destination btree and pager. */
    if( (dest = p->dest)==0 ){
      dest = __find_btree(p->dest_db, p->dest_db, p->z_dest_db);
    }
    if( dest==0 ){
      rc = DBSQL_ERROR;
    }else{
      dest_pager = __sm_pager(dest);
    }

    /* If the destination database has not yet been locked (i.e. if this
    ** is the first call to backup_step() for the current backup operation),
    ** try to set its page size to the same as the source database. This
    ** is especially important on __zip_vfs systems, as in that case it is
    ** not possible to create a database file that uses one page size by
    ** writing to it with another.  */
    if( p->dest_locked==0 && rc==DBSQL_OK 
     && __set_dest_pgsz(dest, p->src)==DBSQL_NOMEM
    ){
      rc = DBSQL_NOMEM;
    }

    /* Lock the destination database, if it is not locked already. */
    if( DBSQL_OK==rc && p->dest_locked==0
     && DBSQL_OK==(rc = __sm_begin_trans(dest, 2,
                                                (int*)&p->i_dest_schema)) 
    ){
      p->dest_locked = 1;
      p->dest = dest;
    }

    /* Do not allow backup if the destination database is in WAL mode
    ** and the page sizes are different between source and destination */
    if( rc==DBSQL_OK ){
      pgsz_src = __sm_get_page_size(p->src);
      pgsz_dest = __sm_get_page_size(p->dest);
      dest_mode = __sm_pager_get_journal_mode(__sm_pager(p->dest));
      if( (dest_mode==PAGER_JOURNALMODE_WAL || __sm_pager_is_memdb(dest_pager))
        && pgsz_src!=pgsz_dest 
      ){
        rc = DBSQL_READONLY;
      }
    }
  
    /* Now that there is a read-lock on the source database, query the
    ** source pager for the number of pages in the database.
    */
    n_src_page = (int)__sm_last_page(p->src);
    assert( n_src_page>=0 );
    for(ii=0; (n_page<0 || ii<n_page) && p->i_next<=(pgno_t)n_src_page && !rc; ii++){
      const pgno_t i_src_pg = p->i_next;                 /* Source page number */
      if( i_src_pg!=PENDING_BYTE_PAGE(p->src->bt) ){
        db_page_t *src_pg;                             /* Source page object */
        rc = __sm_pager_get(src_pager, i_src_pg, &src_pg,PAGER_GET_READONLY);
        if( rc==DBSQL_OK ){
          rc = __backup_one_page(p, i_src_pg, __sm_pager_get_data(src_pg), 0);
          __sm_pager_unref(src_pg);
        }
      }
      p->i_next++;
    }
    if( rc==DBSQL_OK ){
      p->n_pagecount = n_src_page;
      p->n_remaining = n_src_page+1-p->i_next;
      if( p->i_next>(pgno_t)n_src_page ){
        rc = DBSQL_DONE;
      }else if( !p->is_attached ){
        __attach_backup_object(p);
      }
    }
  
    /* Update the schema version field in the destination database. This
    ** is to make sure that the schema-version really does change in
    ** the case where the source and destination databases have the
    ** same schema version.
    */
    if( rc==DBSQL_DONE ){
      if( n_src_page==0 ){
        rc = __sm_new_db(p->dest);
        n_src_page = 1;
      }
      if( rc==DBSQL_OK || rc==DBSQL_DONE ){
        rc = __sm_update_meta(p->dest,1,p->i_dest_schema+1);
      }
      if( rc==DBSQL_OK ){
        if( p->dest_db ){
          __reset_all_schemas_of_connection(p->dest_db);
        }
        if( dest_mode==PAGER_JOURNALMODE_WAL ){
          rc = __sm_set_version(p->dest, 2);
        }
      }
      if( rc==DBSQL_OK ){
        int n_dest_truncate;
        /* Set n_dest_truncate to the final number of pages in the destination
        ** database. The complication here is that the destination page
        ** size may be different to the source page size. 
        **
        ** If the source page size is smaller than the destination page size, 
        ** round up. In this case the call to __dbsql_os_truncate() below will
        ** fix the size of the file. However it is important to call
        ** __sm_pager_truncate_image() here so that any pages in the 
        ** destination file that lie beyond the n_dest_truncate page mark are
        ** journalled by __pager_commit_phase_one() before they are destroyed
        ** by the file truncation.
        */
        assert( pgsz_src==__sm_get_page_size(p->src) );
        assert( pgsz_dest==__sm_get_page_size(p->dest) );
        if( pgsz_src<pgsz_dest ){
          int ratio = pgsz_dest/pgsz_src;
          n_dest_truncate = (n_src_page+ratio-1)/ratio;
          if( n_dest_truncate==(int)PENDING_BYTE_PAGE(p->dest->bt) ){
            n_dest_truncate--;
          }
        }else{
          n_dest_truncate = n_src_page * (pgsz_src/pgsz_dest);
        }
        assert( n_dest_truncate>0 );

        if( pgsz_src<pgsz_dest ){
          /* If the source page-size is smaller than the destination page-size,
          ** two extra things may need to happen:
          **
          **   * The destination may need to be truncated, and
          **
          **   * Data stored on the pages immediately following the 
          **     pending-byte page in the source database may need to be
          **     copied into the destination database.
          */
          const int64_t i_size = (int64_t)pgsz_src * (int64_t)n_src_page;
          dbsql_file_t * const file = __sm_pager_file(dest_pager);
          pgno_t i_pg;
          int n_dst_page;
          int64_t i_off;
          int64_t i_end;

          assert( file );
          assert( n_dest_truncate==0 
              || (int64_t)n_dest_truncate*(int64_t)pgsz_dest >= i_size || (
                n_dest_truncate==(int)(PENDING_BYTE_PAGE(p->dest->bt)-1)
             && i_size>=PENDING_BYTE && i_size<=PENDING_BYTE+pgsz_dest
          ));

          /* This block ensures that all data required to recreate the original
          ** database has been stored in the journal for dest_pager and the
          ** journal synced to disk. So at this point we may safely modify
          ** the database file in any way, knowing that if a power failure
          ** occurs, the original database will be reconstructed from the 
          ** journal file.  */
          __sm_pager_pagecount(dest_pager, &n_dst_page);
          for(i_pg=n_dest_truncate; rc==DBSQL_OK && i_pg<=(pgno_t)n_dst_page; i_pg++){
            if( i_pg!=PENDING_BYTE_PAGE(p->dest->bt) ){
              db_page_t *pg;
              rc = __sm_pager_get(dest_pager, i_pg, &pg, 0);
              if( rc==DBSQL_OK ){
                rc = __sm_pager_write(pg);
                __sm_pager_unref(pg);
              }
            }
          }
          if( rc==DBSQL_OK ){
            rc = __sm_pager_commit_phase_one(dest_pager, 0, 1);
          }

          /* Write the extra pages and truncate the database file as required */
          i_end = MIN(PENDING_BYTE + pgsz_dest, i_size);
          for(
            i_off=PENDING_BYTE+pgsz_src; 
            rc==DBSQL_OK && i_off<i_end; 
            i_off+=pgsz_src
          ){
            pg_hdr_t *src_pg = 0;
            const pgno_t i_src_pg = (pgno_t)((i_off/pgsz_src)+1);
            rc = __sm_pager_get(src_pager, i_src_pg, &src_pg, 0);
            if( rc==DBSQL_OK ){
              u_int8_t *data = __sm_pager_get_data(src_pg);
              rc = __dbsql_os_write(file, data, pgsz_src, i_off);
            }
            __sm_pager_unref(src_pg);
          }
          if( rc==DBSQL_OK ){
            rc = __backup_truncate_file(file, i_size);
          }

          /* Sync the database file to disk. */
          if( rc==DBSQL_OK ){
            rc = __sm_pager_sync(dest_pager, 0);
          }
        }else{
          __sm_pager_truncate_image(dest_pager, n_dest_truncate);
          rc = __sm_pager_commit_phase_one(dest_pager, 0, 0);
        }
    
        /* Finish committing the transaction to the destination database. */
        if( DBSQL_OK==rc
         && DBSQL_OK==(rc = __sm_commit_phase_two(p->dest, 0))
        ){
          rc = DBSQL_DONE;
        }
      }
    }
  
    /* If close_trans is true, then this function opened a read transaction
    ** on the source database. Close the read transaction here. There is
    ** no need to check the return values of the btree methods here, as
    ** "committing" a read-only transaction cannot fail.
    */
    if( close_trans ){
      TESTONLY( int rc2 );
      TESTONLY( rc2  = ) __sm_commit_phase_one(p->src, 0);
      TESTONLY( rc2 |= ) __sm_commit_phase_two(p->src, 0);
      assert( rc2==DBSQL_OK );
    }
  
    if( rc==DBSQL_IOERR_NOMEM ){
      rc = DBSQL_NOMEM_BKPT;
    }
    p->rc = rc;
  }
  if( p->dest_db ){
    dbsql_mutex_leave(p->dest_db->mutex);
  }
  __sm_leave(p->src);
  dbsql_mutex_leave(p->src_db->mutex);
  return rc;
}

/*
** Release all resources associated with an dbsql_backup_t* handle.
*/
int dbsql_backup_finish(dbsql_backup_t *p){
  dbsql_backup_t **pp;                 /* Ptr to head of pagers backup list */
  DBSQL *src_db;                     /* Source database connection */
  int rc;                              /* Value to return */

  /* Enter the mutexes */
  if( p==0 ) return DBSQL_OK;
  src_db = p->src_db;
  dbsql_mutex_enter(src_db->mutex);
  __sm_enter(p->src);
  if( p->dest_db ){
    dbsql_mutex_enter(p->dest_db->mutex);
  }

  /* Detach this backup from the source pager. */
  if( p->dest_db ){
    p->src->n_backup--;
  }
  if( p->is_attached ){
    pp = __sm_pager_backup_ptr(__sm_pager(p->src));
    assert( pp!=0 );
    while( *pp!=p ){
      pp = &(*pp)->next;
      assert( pp!=0 );
    }
    *pp = p->next;
  }

  /* If a transaction is still open on the sm_t, roll it back. */
  if( p->dest ){
    __sm_rollback(p->dest, DBSQL_OK, 0);
  }

  /* Set the error code of the destination database handle. */
  rc = (p->rc==DBSQL_DONE) ? DBSQL_OK : p->rc;
  if( p->dest_db ){
    __dbsql_error(p->dest_db, rc);

    /* Exit the mutexes and free the backup context structure. */
    __leave_mutex_and_close_zombie(p->dest_db);
  }
  __sm_leave(p->src);
  if( p->dest_db ){
    /* EVIDENCE-OF: R-64852-21591 The dbsql_backup_t object is created by a
    ** call to dbsql_backup_init() and is destroyed by a call to
    ** dbsql_backup_finish(). */
    dbsql_free(p);
  }
  __leave_mutex_and_close_zombie(src_db);
  return rc;
}

/*
** Return the number of pages still to be backed up as of the most recent
** call to dbsql_backup_step().
*/
int dbsql_backup_remaining(dbsql_backup_t *p){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  return p->n_remaining;
}

/*
** Return the total number of pages in the source database as of the most 
** recent call to dbsql_backup_step().
*/
int dbsql_backup_pagecount(dbsql_backup_t *p){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  return p->n_pagecount;
}

/*
** This function is called after the contents of page i_page of the
** source database have been modified. If page i_page has already been 
** copied into the destination database, then the data written to the
** destination is now invalidated. The destination copy of i_page needs
** to be updated with the new data before the backup operation is
** complete.
**
** It is assumed that the mutex associated with the sm_shared_t object
** corresponding to the source database is held when this function is
** called.
*/
static DBSQL_NOINLINE void __backup_update_local(
  dbsql_backup_t *p,
  pgno_t i_page,
  const u_int8_t *data
){
  assert( p!=0 );
  do{
    assert( dbsql_mutex_held(p->src->bt->mutex) );
    if( !__is_fatal_error(p->rc) && i_page<p->i_next ){
      /* The backup process p has already copied page i_page. But now it
      ** has been modified by a transaction on the source pager. Copy
      ** the new data into the backup.
      */
      int rc;
      assert( p->dest_db );
      dbsql_mutex_enter(p->dest_db->mutex);
      rc = __backup_one_page(p, i_page, data, 1);
      dbsql_mutex_leave(p->dest_db->mutex);
      assert( rc!=DBSQL_BUSY && rc!=DBSQL_LOCKED );
      if( rc!=DBSQL_OK ){
        p->rc = rc;
      }
    }
  }while( (p = p->next)!=0 );
}
void __backup_update(dbsql_backup_t *backup, pgno_t i_page, const u_int8_t *data){
  if( backup ) __backup_update_local(backup, i_page, data);
}

/*
** Restart the backup process. This is called when the pager layer
** detects that the database has been modified by an external database
** connection. In this case there is no way of knowing which of the
** pages that have been copied into the destination database are still 
** valid and which are not, so the entire process needs to be restarted.
**
** It is assumed that the mutex associated with the sm_shared_t object
** corresponding to the source database is held when this function is
** called.
*/
void __backup_restart(dbsql_backup_t *backup){
  dbsql_backup_t *p;                   /* Iterator variable */
  for(p=backup; p; p=p->next){
    assert( dbsql_mutex_held(p->src->bt->mutex) );
    p->i_next = 1;
  }
}

#ifndef DBSQL_OMIT_VACUUM
/*
** Copy the complete content of bt_from into bt_to.  A transaction
** must be active for both files.
**
** The size of file to may be reduced by this operation. If anything 
** goes wrong, the transaction on to is rolled back. If successful, the 
** transaction is committed before returning.
*/
int __sm_copy_file(sm_t *to, sm_t *from){
  int rc;
  dbsql_file_t *fd;              /* File descriptor for database to */
  dbsql_backup_t b;
  __sm_enter(to);
  __sm_enter(from);

  assert( __sm_txn_state(to)==DBSQL_TXN_WRITE );
  fd = __sm_pager_file(__sm_pager(to));
  if( fd->methods ){
    int64_t n_byte = __sm_get_page_size(from)*(int64_t)__sm_last_page(from);
    rc = __dbsql_os_file_control(fd, DBSQL_FCNTL_OVERWRITE, &n_byte);
    if( rc==DBSQL_NOTFOUND ) rc = DBSQL_OK;
    if( rc ) goto copy_finished;
  }

  /* Set up an dbsql_backup_t object. dbsql_backup_t.dest_db must be set
  ** to 0. This is used by the implementations of dbsql_backup_step()
  ** and dbsql_backup_finish() to detect that they are being called
  ** from this function, not directly by the user.
  */
  memset(&b, 0, sizeof(b));
  b.src_db = from->db;
  b.src = from;
  b.dest = to;
  b.i_next = 1;

  /* 0x7FFFFFFF is the hard limit for the number of pages in a database
  ** file. By passing this as the number of pages to copy to
  ** dbsql_backup_step(), we can guarantee that the copy finishes 
  ** within a single call (unless an error occurs). The assert() statement
  ** checks this assumption - (p->rc) should be set to either DBSQL_DONE 
  ** or an error code.  */
  dbsql_backup_step(&b, 0x7FFFFFFF);
  assert( b.rc!=DBSQL_OK );

  rc = dbsql_backup_finish(&b);
  if( rc==DBSQL_OK ){
    to->bt->bts_flags &= ~BTS_PAGESIZE_FIXED;
  }else{
    __sm_pager_clear_cache(__sm_pager(b.dest));
  }

  assert( __sm_txn_state(to)!=DBSQL_TXN_WRITE );
copy_finished:
  __sm_leave(from);
  __sm_leave(to);
  return rc;
}
#endif /* DBSQL_OMIT_VACUUM */
