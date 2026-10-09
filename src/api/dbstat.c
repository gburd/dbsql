/*
** 2010 July 12
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
** This file contains an implementation of the "dbstat" virtual table.
**
** The dbstat virtual table is used to extract low-level storage
** information from an dbsql database in order to implement the
** "dbsql_analyzer" utility.  See the ../tool/spaceanal.tcl script
** for an example implementation.
**
** Additional information is available on the "dbstat.html" page of the
** official dbsql documentation.
*/

#include "dbsql_int.h"   /* Requires access to internal data structures */
#if (defined(DBSQL_ENABLE_DBSTAT_VTAB) || defined(DBSQL_TEST)) \
    && !defined(DBSQL_OMIT_VIRTUALTABLE)

/*
** The pager and btree modules arrange objects in memory so that there are
** always approximately 200 bytes of addressable memory following each page
** buffer. This way small buffer overreads caused by corrupt database pages
** do not cause undefined behaviour. This module pads each page buffer
** by the following number of bytes for the same purpose.
*/
#define DBSTAT_PAGE_PADDING_BYTES 256

/*
** Page paths:
** 
**   The value of the 'path' column describes the path taken from the 
**   root-node of the b-tree structure to each page. The value of the 
**   root-node path is '/'.
**
**   The value of the path for the left-most child page of the root of
**   a b-tree is '/000/'. (Btrees store content ordered from left to right
**   so the pages to the left have smaller keys than the pages to the right.)
**   The next to left-most child of the root page is
**   '/001', and so on, each sibling page identified by a 3-digit hex 
**   value. The children of the 451st left-most sibling have paths such
**   as '/1c2/000/, '/1c2/001/' etc.
**
**   Overflow pages are specified by appending a '+' character and a 
**   six-digit hexadecimal value to the path to the cell they are linked
**   from. For example, the three overflow pages in a chain linked from 
**   the left-most cell of the 450th child of the root page are identified
**   by the paths:
**
**      '/1c2/000+000000'         // First page in overflow chain
**      '/1c2/000+000001'         // Second page in overflow chain
**      '/1c2/000+000002'         // Third page in overflow chain
**
**   If the paths are sorted using the BINARY collation sequence, then
**   the overflow pages associated with a cell will appear earlier in the
**   sort-order than its child page:
**
**      '/1c2/000/'               // Left-most child of 451st child of root
*/
static const char __z_dbstat_schema[] = 
  "CREATE TABLE x("
  " name       TEXT,"          /*  0 Name of table or index */
  " path       TEXT,"          /*  1 Path to page from root (NULL for agg) */
  " pageno     INTEGER,"       /*  2 Page number (page count for aggregates) */
  " pagetype   TEXT,"          /*  3 'internal', 'leaf', 'overflow', or NULL */
  " ncell      INTEGER,"       /*  4 Cells on page (0 for overflow) */
  " payload    INTEGER,"       /*  5 Bytes of payload on this page */
  " unused     INTEGER,"       /*  6 Bytes of unused space on this page */
  " mx_payload INTEGER,"       /*  7 Largest payload size of all cells */
  " pgoffset   INTEGER,"       /*  8 Offset of page in file (NULL for agg) */
  " pgsize     INTEGER,"       /*  9 Size of the page (sum for aggregate) */
  " schema     TEXT HIDDEN,"   /* 10 Database schema being analyzed */
  " aggregate  BOOLEAN HIDDEN" /* 11 aggregate info for each table */
  ")"
;

/* Forward reference to data structured used in this module */
typedef struct stat_table_t stat_table_t;
typedef struct stat_cursor_t stat_cursor_t;
typedef struct stat_page_t stat_page_t;
typedef struct stat_cell_t stat_cell_t;

/* Size information for a single cell within a btree page */
struct stat_cell_t {
  int n_local;                     /* Bytes of local payload */
  u_int32_t i_child_pg;                   /* Child node (or 0 if this is a leaf) */
  int n_ovfl;                      /* Entries in ovfls[] */
  u_int32_t *ovfls;                     /* Array of overflow page numbers */
  int n_last_ovfl;                  /* Bytes of payload on final overflow page */
  int i_ovfl;                      /* Iterates through ovfls[] */
};

/* Size information for a single btree page */
struct stat_page_t {
  u_int32_t i_pgno;                      /* Page number */
  u_int8_t *pgs;                        /* Page buffer from dbsql_malloc() */
  int i_cell;                      /* Current cell */
  char *path;                    /* Path to this page */

  /* Variables populated by __stat_decode_page(): */
  u_int8_t flags;                       /* Copy of flags byte */
  int n_cell;                      /* Number of cells on page */
  int n_unused;                    /* Number of unused bytes on page */
  stat_cell_t *cells;                /* Array of parsed cells */
  u_int32_t i_right_child_pg;              /* Right-child page number (or 0) */
  int n_mx_payload;                 /* Largest payload of any cell on the page */
};

/* The cursor for scanning the dbstat virtual table */
struct stat_cursor_t {
  dbsql_vtab_cursor_t base;       /* base class.  MUST BE FIRST! */
  dbsql_stmt_t *stmt;            /* Iterates through set of root pages */
  u_int8_t is_eof;                       /* After stmt has returned DBSQL_DONE */
  u_int8_t is_agg;                       /* Aggregate results for each table */
  int i_db;                        /* schema_t used for this query */

  stat_page_t pages[32];             /* Pages in path to current page */
  int i_page;                      /* Current entry in pages[] */

  /* Values to return. */
  u_int32_t i_pageno;                    /* Value of 'pageno' column */
  char *name;                    /* Value of 'name' column */
  char *path;                    /* Value of 'path' column */
  char *pagetype;                /* Value of 'pagetype' column */
  int n_page;                      /* Number of pages in current btree */
  int n_cell;                      /* Value of 'ncell' column */
  int n_mx_payload;                 /* Value of 'mx_payload' column */
  int64_t n_unused;                    /* Value of 'unused' column */
  int64_t n_payload;                   /* Value of 'payload' column */
  int64_t i_offset;                    /* Value of 'pg_offset' column */
  int64_t sz_page;                     /* Value of 'pg_size' column */
};

/* An instance of the DBSTAT virtual table */
struct stat_table_t {
  dbsql_vtab_t base;              /* base class.  MUST BE FIRST! */
  DBSQL *db;                    /* Database connection that owns this vtab */
  int i_db;                        /* index_t of database to analyze */
};

#ifndef get2byte
# define get2byte(x)   ((x)[0]<<8 | (x)[1])
#endif

/*
** Connect to or create a new DBSTAT virtual table.
*/
static int __stat_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  stat_table_t *tab = 0;
  int rc = DBSQL_OK;
  int i_db;
  (void)aux;

  if( argc>=4 ){
    Token nm;
    __token_init(&nm, (char*)argv[3]);
    i_db = __find_db(db, &nm);
    if( i_db<0 ){
      *err_p = dbsql_mprintf("no such database: %s", argv[3]);
      return DBSQL_ERROR;
    }
  }else{
    i_db = 0;
  }
  dbsql_vtab_config(db, DBSQL_VTAB_DIRECTONLY);
  rc = dbsql_declare_vtab(db, __z_dbstat_schema);
  if( rc==DBSQL_OK ){
    tab = (stat_table_t *)dbsql_malloc64(sizeof(stat_table_t));
    if( tab==0 ) rc = DBSQL_NOMEM_BKPT;
  }

  assert( rc==DBSQL_OK || tab==0 );
  if( rc==DBSQL_OK ){
    memset(tab, 0, sizeof(stat_table_t));
    tab->db = db;
    tab->i_db = i_db;
  }

  *vtab = (dbsql_vtab_t*)tab;
  return rc;
}

/*
** Disconnect from or destroy the DBSTAT virtual table.
*/
static int __stat_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Compute the best query strategy and return the result in idx_num.
**
**   idx_num-Bit        Meaning
**   ----------        ----------------------------------------------
**      0x01           There is a schema=? term in the WHERE clause
**      0x02           There is a name=? term in the WHERE clause
**      0x04           There is an aggregate=? term in the WHERE clause
**      0x08           Output should be ordered by name and path
*/
static int __stat_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int i;
  int i_schema = -1;
  int i_name = -1;
  int i_agg = -1;
  (void)tab;

  /* Look for a valid schema=? constraint.  If found, change the idx_num to
  ** 1 and request the value of that constraint be sent to filter.  And
  ** lower the cost estimate to encourage the constrained version to be
  ** used.
  */
  for(i=0; i<idx_info->n_constraint; i++){
    if( idx_info->constraints[i].op!=DBSQL_INDEX_CONSTRAINT_EQ ) continue;
    if( idx_info->constraints[i].usable==0 ){
      /* Force DBSTAT table should always be the right-most table in a join */
      return DBSQL_CONSTRAINT;
    }
    switch( idx_info->constraints[i].i_column ){
      case 0: {    /* name */
        i_name = i;
        break;
      }
      case 10: {   /* schema */
        i_schema = i;
        break;
      }
      case 11: {   /* aggregate */
        i_agg = i;
        break;
      }
    }
  }
  i = 0;
  if( i_schema>=0 ){
    idx_info->constraint_usages[i_schema].argv_index = ++i;
    idx_info->constraint_usages[i_schema].omit = 1;
    idx_info->idx_num |= 0x01;
  }
  if( i_name>=0 ){
    idx_info->constraint_usages[i_name].argv_index = ++i;
    idx_info->idx_num |= 0x02;
  }
  if( i_agg>=0 ){
    idx_info->constraint_usages[i_agg].argv_index = ++i;
    idx_info->idx_num |= 0x04;
  }
  idx_info->estimated_cost = 1.0;

  /* Records are always returned in ascending order of (name, path). 
  ** If this will satisfy the client, set the order_by_consumed flag so that 
  ** dbsql does not do an external sort.
  */
  if( ( idx_info->n_order_by==1
     && idx_info->order_bies[0].i_column==0
     && idx_info->order_bies[0].desc==0
     ) ||
      ( idx_info->n_order_by==2
     && idx_info->order_bies[0].i_column==0
     && idx_info->order_bies[0].desc==0
     && idx_info->order_bies[1].i_column==1
     && idx_info->order_bies[1].desc==0
     )
  ){
    idx_info->order_by_consumed = 1;
    idx_info->idx_num |= 0x08;
  }
  idx_info->idx_flags |= DBSQL_INDEX_SCAN_HEX;

  return DBSQL_OK;
}

/*
** Open a new DBSTAT cursor.
*/
static int __stat_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  stat_table_t *tab = (stat_table_t *)v_tab;
  stat_cursor_t *csr;

  csr = (stat_cursor_t *)dbsql_malloc64(sizeof(stat_cursor_t));
  if( csr==0 ){
    return DBSQL_NOMEM_BKPT;
  }else{
    memset(csr, 0, sizeof(stat_cursor_t));
    csr->base.vtab = v_tab;
    csr->i_db = tab->i_db;
  }

  *cursor = (dbsql_vtab_cursor_t *)csr;
  return DBSQL_OK;
}

static void __stat_clear_cells(stat_page_t *p){
  int i;
  if( p->cells ){
    for(i=0; i<p->n_cell; i++){
      dbsql_free(p->cells[i].ovfls);
    }
    dbsql_free(p->cells);
  }
  p->n_cell = 0;
  p->cells = 0;
}

static void __stat_clear_page(stat_page_t *p){
  u_int8_t *pgs = p->pgs;
  __stat_clear_cells(p);
  dbsql_free(p->path);
  memset(p, 0, sizeof(stat_page_t));
  p->pgs = pgs;
}

static void __stat_reset_csr(stat_cursor_t *csr){
  int i;
  /* In some circumstances, specifically if an OOM has occurred, the call
  ** to dbsql_reset() may cause the pager to be reset (emptied). It is
  ** important that __stat_clear_page() is called to free any page refs before
  ** this happens. dbsqlfuzz 9ed3e4e3816219d3509d711636c38542bf3f40b1. */
  for(i=0; i<ARRAY_SIZE(csr->pages); i++){
    __stat_clear_page(&csr->pages[i]);
    dbsql_free(csr->pages[i].pgs);
    csr->pages[i].pgs = 0;
  }
  dbsql_reset(csr->stmt);
  csr->i_page = 0;
  dbsql_free(csr->path);
  csr->path = 0;
  csr->is_eof = 0;
}

/* Resize the space-used counters inside of the cursor */
static void __stat_reset_counts(stat_cursor_t *csr){
  csr->n_cell = 0;
  csr->n_mx_payload = 0;
  csr->n_unused = 0;
  csr->n_payload = 0;
  csr->sz_page = 0;
  csr->n_page = 0;
}

/*
** Close a DBSTAT cursor.
*/
static int __stat_close(dbsql_vtab_cursor_t *cursor){
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  __stat_reset_csr(csr);
  dbsql_finalize(csr->stmt);
  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** For a single cell on a btree page, compute the number of bytes of
** content (payload) stored on that page.  That is to say, compute the
** number of bytes of content not found on overflow pages.
*/
static int __get_local_payload(
  int n_usable,                    /* Usable bytes per page */
  u_int8_t flags,                       /* Page flags */
  int n_total                      /* Total record (payload) size */
){
  int n_local;
  int n_min_local;
  int n_max_local;
 
  if( flags==0x0D ){              /* table_t leaf node */
    n_min_local = (n_usable - 12) * 32 / 255 - 23;
    n_max_local = n_usable - 35;
  }else{                          /* index_t interior and leaf nodes */
    n_min_local = (n_usable - 12) * 32 / 255 - 23;
    n_max_local = (n_usable - 12) * 64 / 255 - 23;
  }

  n_local = n_min_local + (n_total - n_min_local) % (n_usable - 4);
  if( n_local>n_max_local ) n_local = n_min_local;
  return n_local;
}

/* Populate the stat_page_t object with information about the all
** cells found on the page currently under analysis.
*/
static int __stat_decode_page(sm_t *bt, stat_page_t *p){
  int n_unused;
  int i_off;
  int n_hdr;
  int is_leaf;
  int sz_page;

  u_int8_t *data = p->pgs;
  u_int8_t *hdrs = &data[p->i_pgno==1 ? 100 : 0];

  p->flags = hdrs[0];
  if( p->flags==0x0A || p->flags==0x0D ){
    is_leaf = 1;
    n_hdr = 8;
  }else if( p->flags==0x05 || p->flags==0x02 ){
    is_leaf = 0;
    n_hdr = 12;
  }else{
    goto stat_page_is_corrupt;
  }
  if( p->i_pgno==1 ) n_hdr += 100;
  p->n_cell = get2byte(&hdrs[3]);
  p->n_mx_payload = 0;
  sz_page = __sm_get_page_size(bt);

  n_unused = get2byte(&hdrs[5]) - n_hdr - 2*p->n_cell;
  n_unused += (int)hdrs[7];
  i_off = get2byte(&hdrs[1]);
  while( i_off ){
    int i_next;
    if( i_off>=sz_page ) goto stat_page_is_corrupt;
    n_unused += get2byte(&data[i_off+2]);
    i_next = get2byte(&data[i_off]);
    if( i_next<i_off+4 && i_next>0 ) goto stat_page_is_corrupt;
    i_off = i_next;
  }
  p->n_unused = n_unused;
  p->i_right_child_pg = is_leaf ? 0 : __get4byte(&hdrs[8]);

  if( p->n_cell ){
    int i;                        /* Used to iterate through cells */
    int n_usable;                  /* Usable bytes per page */

    __sm_enter(bt);
    n_usable = sz_page - __sm_get_reserve_no_mutex(bt);
    __sm_leave(bt);
    p->cells = dbsql_malloc64((p->n_cell+1) * sizeof(stat_cell_t));
    if( p->cells==0 ) return DBSQL_NOMEM_BKPT;
    memset(p->cells, 0, (p->n_cell+1) * sizeof(stat_cell_t));

    for(i=0; i<p->n_cell; i++){
      stat_cell_t *cell = &p->cells[i];

      i_off = get2byte(&data[n_hdr+i*2]);
      if( i_off<n_hdr || i_off>=sz_page ) goto stat_page_is_corrupt;
      if( !is_leaf ){
        cell->i_child_pg = __get4byte(&data[i_off]);
        i_off += 4;
      }
      if( p->flags==0x05 ){
        /* A table interior node. n_payload==0. */
      }else{
        u_int32_t n_payload;             /* Bytes of payload total (local+overflow) */
        int n_local;               /* Bytes of payload stored locally */
        i_off += GET_VARINT32(&data[i_off], n_payload);
        if( p->flags==0x0D ){
          u_int64_t dummy;
          i_off += __get_varint(&data[i_off], &dummy);
        }
        if( n_payload>(u_int32_t)p->n_mx_payload ) p->n_mx_payload = n_payload;
        n_local = __get_local_payload(n_usable, p->flags, n_payload);
        if( n_local<0 ) goto stat_page_is_corrupt;
        cell->n_local = n_local;
        assert( n_payload>=(u_int32_t)n_local );
        assert( n_local<=(n_usable-35) );
        if( n_payload>(u_int32_t)n_local ){
          int j;
          int n_ovfl = ((n_payload - n_local) + n_usable-4 - 1) / (n_usable - 4);
          if( i_off+n_local+4>n_usable || n_payload>0x7fffffff ){
            goto stat_page_is_corrupt;
          }
          cell->n_last_ovfl = (n_payload-n_local) - (n_ovfl-1) * (n_usable-4);
          cell->n_ovfl = n_ovfl;
          cell->ovfls = dbsql_malloc64(sizeof(u_int32_t)*n_ovfl);
          if( cell->ovfls==0 ) return DBSQL_NOMEM_BKPT;
          cell->ovfls[0] = __get4byte(&data[i_off+n_local]);
          for(j=1; j<n_ovfl; j++){
            int rc;
            u_int32_t i_prev = cell->ovfls[j-1];
            db_page_t *pg = 0;
            rc = __sm_pager_get(__sm_pager(bt), i_prev, &pg, 0);
            if( rc!=DBSQL_OK ){
              assert( pg==0 );
              return rc;
            } 
            cell->ovfls[j] = __get4byte(__sm_pager_get_data(pg));
            __sm_pager_unref(pg);
          }
        }
      }
    }
  }

  return DBSQL_OK;

stat_page_is_corrupt:
  p->flags = 0;
  __stat_clear_cells(p);
  return DBSQL_OK;
}

/*
** Populate the csr->i_offset and csr->sz_page member variables. Based on
** the current value of csr->i_pageno.
*/
static void __stat_size_and_offset(stat_cursor_t *csr){
  stat_table_t *tab = (stat_table_t *)((dbsql_vtab_cursor_t *)csr)->vtab;
  sm_t *bt = tab->db->dbs[tab->i_db].bt;
  pager_t *pager = __sm_pager(bt);
  dbsql_file_t *fd;
  int64_t x[2];

  /* If connected to a ZIPVFS backend, find the page size and
  ** offset from ZIPVFS.
  */
  fd = __sm_pager_file(pager);
  x[0] = csr->i_pageno;
  if( __dbsql_os_file_control(fd, 230440, &x)==DBSQL_OK ){
    csr->i_offset = x[0];
    csr->sz_page += x[1];
  }else{
    /* Not ZIPVFS: The default page size and offset */
    csr->sz_page += __sm_get_page_size(bt);
    csr->i_offset = (int64_t)csr->sz_page * (csr->i_pageno - 1);
  }
}

/*
** Load a copy of the page data for page i_pg into the buffer belonging
** to page object pg. Allocate the buffer if necessary. Return DBSQL_OK
** if successful, or an dbsql error code otherwise.
*/
static int __stat_get_page(
  sm_t *bt,                     /* Load page from this b-tree */
  u_int32_t i_pg,                        /* Page number to load */
  stat_page_t *pg                   /* Load page into this object */
){
  int pgsz = __sm_get_page_size(bt);
  db_page_t *db_page = 0;
  int rc;

  if( pg->pgs==0 ){
    pg->pgs = (u_int8_t*)dbsql_malloc(pgsz + DBSTAT_PAGE_PADDING_BYTES);
    if( pg->pgs==0 ){
      return DBSQL_NOMEM_BKPT;
    }
    memset(&pg->pgs[pgsz], 0, DBSTAT_PAGE_PADDING_BYTES);
  }

  rc = __sm_pager_get(__sm_pager(bt), i_pg, &db_page, 0);
  if( rc==DBSQL_OK ){
    const u_int8_t *a = __sm_pager_get_data(db_page);
    memcpy(pg->pgs, a, pgsz);
    __sm_pager_unref(db_page);
  }

  return rc;
}

/*
** Move a DBSTAT cursor to the next entry.  Normally, the next
** entry will be the next page, but in aggregated mode (csr->is_agg!=0),
** the next entry is the next btree.
*/
static int __stat_next(dbsql_vtab_cursor_t *cursor){
  int rc;
  int n_payload;
  char *z;
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  stat_table_t *tab = (stat_table_t *)cursor->vtab;
  sm_t *bt = tab->db->dbs[csr->i_db].bt;
  pager_t *pager = __sm_pager(bt);

  dbsql_free(csr->path);
  csr->path = 0;

stat_next_restart:
  if( csr->i_page<0 ){
    /* Start measuring space on the next btree */
    __stat_reset_counts(csr);
    rc = dbsql_step(csr->stmt);
    if( rc==DBSQL_ROW ){
      int n_page;
      u_int32_t i_root = (u_int32_t)dbsql_column_int64(csr->stmt, 1);
      __sm_pager_pagecount(pager, &n_page);
      if( n_page==0 ){
        csr->is_eof = 1;
        return dbsql_reset(csr->stmt);
      }
      rc = __stat_get_page(bt, i_root, &csr->pages[0]);
      csr->pages[0].i_pgno = i_root;
      csr->pages[0].i_cell = 0;
      if( !csr->is_agg ){
        csr->pages[0].path = z = dbsql_mprintf("/");
        if( z==0 ) rc = DBSQL_NOMEM_BKPT;
      }
      csr->i_page = 0;
      csr->n_page = 1;
    }else{
      csr->is_eof = 1;
      return dbsql_reset(csr->stmt);
    }
  }else{
    /* Continue analyzing the btree previously started */
    stat_page_t *p = &csr->pages[csr->i_page];
    if( !csr->is_agg ) __stat_reset_counts(csr);
    while( p->i_cell<p->n_cell ){
      stat_cell_t *cell = &p->cells[p->i_cell];
      while( cell->i_ovfl<cell->n_ovfl ){
        int n_usable, i_ovfl;
        __sm_enter(bt);
        n_usable = __sm_get_page_size(bt) - 
                        __sm_get_reserve_no_mutex(bt);
        __sm_leave(bt);
        csr->n_page++;
        __stat_size_and_offset(csr);
        if( cell->i_ovfl<cell->n_ovfl-1 ){
          csr->n_payload += n_usable - 4;
        }else{
          csr->n_payload += cell->n_last_ovfl;
          csr->n_unused += n_usable - 4 - cell->n_last_ovfl;
        }
        i_ovfl = cell->i_ovfl;
        cell->i_ovfl++;
        if( !csr->is_agg ){
          csr->name = (char *)dbsql_column_text(csr->stmt, 0);
          csr->i_pageno = cell->ovfls[i_ovfl];
          csr->pagetype = "overflow";
          csr->path = z = dbsql_mprintf(
              "%s%.3x+%.6x", p->path, p->i_cell, i_ovfl
          );
          return z==0 ? DBSQL_NOMEM_BKPT : DBSQL_OK;
        }
      }
      if( p->i_right_child_pg ) break;
      p->i_cell++;
    }

    if( !p->i_right_child_pg || p->i_cell>p->n_cell ){
      __stat_clear_page(p);
      csr->i_page--;
      if( csr->is_agg && csr->i_page<0 ){
        /* label-__stat_next-done:  When computing aggregate space usage over
        ** an entire btree, this is the exit point from this function */
        return DBSQL_OK;
      }
      goto stat_next_restart; /* Tail recursion */
    }
    csr->i_page++;
    if( csr->i_page>=ARRAY_SIZE(csr->pages) ){
      __stat_reset_csr(csr);
      return DBSQL_CORRUPT_BKPT;
    }
    assert( p==&csr->pages[csr->i_page-1] );

    if( p->i_cell==p->n_cell ){
      p[1].i_pgno = p->i_right_child_pg;
    }else{
      p[1].i_pgno = p->cells[p->i_cell].i_child_pg;
    }
    rc = __stat_get_page(bt, p[1].i_pgno, &p[1]);
    csr->n_page++;
    p[1].i_cell = 0;
    if( !csr->is_agg ){
      p[1].path = z = dbsql_mprintf("%s%.3x/", p->path, p->i_cell);
      if( z==0 ) rc = DBSQL_NOMEM_BKPT;
    }
    p->i_cell++;
  }


  /* Populate the stat_cursor_t fields with the values to be returned
  ** by the column() and rowid() methods.
  */
  if( rc==DBSQL_OK ){
    int i;
    stat_page_t *p = &csr->pages[csr->i_page];
    csr->name = (char *)dbsql_column_text(csr->stmt, 0);
    csr->i_pageno = p->i_pgno;

    rc = __stat_decode_page(bt, p);
    if( rc==DBSQL_OK ){
      __stat_size_and_offset(csr);

      switch( p->flags ){
        case 0x05:             /* table internal */
        case 0x02:             /* index internal */
          csr->pagetype = "internal";
          break;
        case 0x0D:             /* table leaf */
        case 0x0A:             /* index leaf */
          csr->pagetype = "leaf";
          break;
        default:
          csr->pagetype = "corrupted";
          break;
      }
      csr->n_cell += p->n_cell;
      csr->n_unused += p->n_unused;
      if( p->n_mx_payload>csr->n_mx_payload ) csr->n_mx_payload = p->n_mx_payload;
      if( !csr->is_agg ){
        csr->path = z = dbsql_mprintf("%s", p->path);
        if( z==0 ) rc = DBSQL_NOMEM_BKPT;
      }
      n_payload = 0;
      for(i=0; i<p->n_cell; i++){
        n_payload += p->cells[i].n_local;
      }
      csr->n_payload += n_payload;

      /* If computing aggregate space usage by btree, continue with the
      ** next page.  The loop will exit via the return at label-__stat_next-done
      */
      if( csr->is_agg ) goto stat_next_restart;
    }
  }

  return rc;
}

static int __stat_eof(dbsql_vtab_cursor_t *cursor){
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  return csr->is_eof;
}

/* Initialize a cursor according to the query plan idx_num using the
** arguments in argv[0].  See __stat_best_index() for a description of the
** meaning of the bits in idx_num.
*/
static int __stat_filter(
  dbsql_vtab_cursor_t *cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  stat_table_t *tab = (stat_table_t*)(cursor->vtab);
  dbsql_str_t *sql;      /* Query of btrees to analyze */
  char *z_sql;             /* String value of sql */
  int i_arg = 0;           /* Count of argv[] parameters used so far */
  int rc = DBSQL_OK;     /* Result of this operation */
  const char *name = 0;  /* Only provide analysis of this table */
  (void)argc;
  (void)idx_str;

  __stat_reset_csr(csr);
  dbsql_finalize(csr->stmt);
  csr->stmt = 0;
  if( idx_num & 0x01 ){
    /* schema=? constraint is present.  Get its value */
    const char *dbase = (const char*)dbsql_value_text(argv[i_arg++]);
    csr->i_db = __find_db_name(tab->db, dbase);
    if( csr->i_db<0 ){
      csr->i_db = 0;
      csr->is_eof = 1;
      return DBSQL_OK;
    }
  }else{
    csr->i_db = tab->i_db;
  }
  if( idx_num & 0x02 ){
    /* name=? constraint is present */
    name = (const char*)dbsql_value_text(argv[i_arg++]);
  }
  if( idx_num & 0x04 ){
    /* aggregate=? constraint is present */
    csr->is_agg = dbsql_value_double(argv[i_arg++])!=0.0;
  }else{
    csr->is_agg = 0;
  }
  sql = dbsql_str_new(tab->db);
  dbsql_str_appendf(sql,
      "SELECT * FROM ("
        "SELECT 'dbsql_schema' AS name,1 AS rootpage,'table' AS type"
        " UNION ALL "
        "SELECT name,rootpage,type"
        " FROM \"%w\".dbsql_schema WHERE rootpage!=0)",
      tab->db->dbs[csr->i_db].db_s_name);
  if( name ){
    dbsql_str_appendf(sql, "WHERE name=%Q", name);
  }
  if( idx_num & 0x08 ){
    dbsql_str_appendf(sql, " ORDER BY name");
  }
  z_sql = dbsql_str_finish(sql);
  if( z_sql==0 ){
    return DBSQL_NOMEM_BKPT;
  }else{
    rc = dbsql_prepare_v2(tab->db, z_sql, -1, &csr->stmt, 0);
    dbsql_free(z_sql);
  }

  if( rc==DBSQL_OK ){
    csr->i_page = -1;
    rc = __stat_next(cursor);
  }
  return rc;
}

static int __stat_column(
  dbsql_vtab_cursor_t *cursor, 
  dbsql_func_t *ctx, 
  int i
){
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  switch( i ){
    case 0:            /* name */
      dbsql_result_text(ctx, csr->name, -1, DBSQL_TRANSIENT);
      break;
    case 1:            /* path */
      if( !csr->is_agg ){
        dbsql_result_text(ctx, csr->path, -1, DBSQL_TRANSIENT);
      }
      break;
    case 2:            /* pageno */
      if( csr->is_agg ){
        dbsql_result_int64(ctx, csr->n_page);
      }else{
        dbsql_result_int64(ctx, csr->i_pageno);
      }
      break;
    case 3:            /* pagetype */
      if( !csr->is_agg ){
        dbsql_result_text(ctx, csr->pagetype, -1, DBSQL_STATIC);
      }
      break;
    case 4:            /* ncell */
      dbsql_result_int64(ctx, csr->n_cell);
      break;
    case 5:            /* payload */
      dbsql_result_int64(ctx, csr->n_payload);
      break;
    case 6:            /* unused */
      dbsql_result_int64(ctx, csr->n_unused);
      break;
    case 7:            /* mx_payload */
      dbsql_result_int64(ctx, csr->n_mx_payload);
      break;
    case 8:            /* pgoffset */
      if( !csr->is_agg ){
        dbsql_result_int64(ctx, csr->i_offset);
      }
      break;
    case 9:            /* pgsize */
      dbsql_result_int64(ctx, csr->sz_page);
      break;
    case 10: {         /* schema */
      DBSQL *db = dbsql_context_db_handle(ctx);
      int i_db = csr->i_db;
      dbsql_result_text(ctx, db->dbs[i_db].db_s_name, -1, DBSQL_STATIC);
      break;
    }
    default: {         /* aggregate */
      dbsql_result_int(ctx, csr->is_agg);
      break;
    }
  }
  return DBSQL_OK;
}

static int __stat_rowid(dbsql_vtab_cursor_t *cursor, int64_t *rowid){
  stat_cursor_t *csr = (stat_cursor_t *)cursor;
  *rowid = csr->i_pageno;
  return DBSQL_OK;
}

/*
** Invoke this routine to register the "dbstat" virtual table module
*/
int __dbstat_register(DBSQL *db){
  static dbsql_module_t dbstat_module = {
    0,                            /* i_version */
    __stat_connect,                  /* create */
    __stat_connect,                  /* connect */
    __stat_best_index,                /* best_index */
    __stat_disconnect,               /* disconnect */
    __stat_disconnect,               /* destroy */
    __stat_open,                     /* x_open - open a cursor */
    __stat_close,                    /* x_close - close a cursor */
    __stat_filter,                   /* filter - configure scan constraints */
    __stat_next,                     /* next - advance a cursor */
    __stat_eof,                      /* eof - check for end of scan */
    __stat_column,                   /* column - read data */
    __stat_rowid,                    /* rowid - read data */
    0,                            /* update */
    0,                            /* begin */
    0,                            /* x_sync */
    0,                            /* commit */
    0,                            /* rollback */
    0,                            /* find_method */
    0,                            /* rename */
    0,                            /* savepoint */
    0,                            /* release */
    0,                            /* rollback_to */
    0,                            /* shadow_name */
    0                             /* integrity */
  };
  return dbsql_create_module(db, "dbstat", &dbstat_module, 0);
}
#elif defined(DBSQL_ENABLE_DBSTAT_VTAB)
int __dbstat_register(DBSQL *db){ return DBSQL_OK; }
#endif /* DBSQL_ENABLE_DBSTAT_VTAB */
