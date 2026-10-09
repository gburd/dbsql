/*
** 2009 November 25
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
** This file contains code used to insert the values of host parameters
** (aka "wildcards") into the SQL text output by dbsql_trace().
**
** The vdbe_t parse-tree explainer is also found here.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

#ifndef DBSQL_OMIT_TRACE

/*
** sql is a zero-terminated string of UTF-8 SQL text.  Return the number of
** bytes in this text up to but excluding the first character in
** a host parameter.  If the text contains no host parameters, return
** the total number of bytes in the text.
*/
static int64_t __find_next_host_parameter(const char *sql, int64_t *pn_token){
  int token_type;
  int64_t n_total = 0;
  int64_t n;

  *pn_token = 0;
  while( sql[0] ){
    n = __get_token((u_int8_t*)sql, &token_type);
    assert( n>0 && token_type!=TK_ILLEGAL );
    if( token_type==TK_VARIABLE ){
      *pn_token = n;
      break;
    }
    n_total += n;
    sql += n;
  }
  return n_total;
}

/*
** This function returns a pointer to a nul-terminated string in memory
** obtained from __db_malloc(). If DBSQL.n_vdbe_exec is 1, then the
** string contains a copy of raw_sql but with host parameters expanded to 
** their current bindings. Or, if DBSQL.n_vdbe_exec is greater than 1, 
** then the returned string holds a copy of raw_sql with "-- " prepended
** to each line of text.
**
** If the DBSQL_TRACE_SIZE_LIMIT macro is defined to an integer, then
** then long strings and blobs are truncated to that many bytes.  This
** can be used to prevent unreasonably large trace strings when dealing
** with large (multi-megabyte) strings and blobs.
**
** The calling function is responsible for making sure the memory returned
** is eventually freed.
**
** ALGORITHM:  Scan the input string looking for host parameters in any of
** these forms:  ?, ?N, $A, @A, :A.  Take care to avoid text within
** string literals, quoted identifier names, and comments.  For text forms,
** the host parameter index is found by scanning the prepared
** statement for the corresponding OP_Variable opcode.  Once the host
** parameter index is known, locate the value in p->vars[].  Then render
** the value as a literal in place of the host parameter name.
*/
char *__vdbe_expand_sql(
  vdbe_t *p,                 /* The prepared statement being evaluated */
  const char *raw_sql      /* Raw text of the SQL statement */
){
  DBSQL *db;             /* The database connection */
  int idx = 0;             /* index_t of a host parameter */
  int nextIndex = 1;       /* index_t of next ? host parameter */
  int64_t n;                   /* Length of a token prefix */
  int64_t n_token;              /* Length of the parameter token */
  int i;                   /* Loop counter */
  mem_t *var;               /* Value of a host parameter */
  str_accum_t out;            /* Accumulate the output here */
#ifndef DBSQL_OMIT_UTF16
  mem_t utf8;                /* Used to convert UTF16 into UTF8 for display */
#endif

  db = p->db;
  __str_accum_init(&out, 0, 0, 0, db->limits[DBSQL_LIMIT_LENGTH]);
  if( db->n_vdbe_exec>1 ){
    while( *raw_sql ){
      const char *start = raw_sql;
      while( *(raw_sql++)!='\n' && *raw_sql );
      dbsql_str_append(&out, "-- ", 3);
      assert( (raw_sql - start) > 0 );
      dbsql_str_append(&out, start, (int)(raw_sql-start));
    }
  }else if( p->n_var==0 ){
    dbsql_str_append(&out, raw_sql, __strlen30(raw_sql));
  }else{
    while( raw_sql[0] ){
      n = __find_next_host_parameter(raw_sql, &n_token);
      assert( n>0 );
      dbsql_str_append(&out, raw_sql, n);
      raw_sql += n;
      assert( raw_sql[0] || n_token==0 );
      if( n_token==0 ) break;
      if( raw_sql[0]=='?' ){
        if( n_token>1 ){
          assert( __isdigit(raw_sql[1]) );
          __get_int32(&raw_sql[1], &idx);
        }else{
          idx = nextIndex;
        }
      }else{
        assert( raw_sql[0]==':' || raw_sql[0]=='$' ||
                raw_sql[0]=='@' || raw_sql[0]=='#' );
        testcase( raw_sql[0]==':' );
        testcase( raw_sql[0]=='$' );
        testcase( raw_sql[0]=='@' );
        testcase( raw_sql[0]=='#' );
        idx = __vdbe_parameter_index(p, raw_sql, n_token);
        assert( idx>0 );
      }
      raw_sql += n_token;
      nextIndex = MAX(idx + 1, nextIndex);
      assert( idx>0 && idx<=p->n_var );
      var = &p->vars[idx-1];
      if( var->flags & MEM_Null ){
        dbsql_str_append(&out, "NULL", 4);
      }else if( var->flags & (MEM_Int|MEM_IntReal) ){
        dbsql_str_appendf(&out, "%lld", var->u.i);
      }else if( var->flags & MEM_Real ){
        dbsql_str_appendf(&out, "%!.15g", var->u.r);
      }else if( var->flags & MEM_Str ){
        int n_out;  /* Number of bytes of the string text to include in output */
#ifndef DBSQL_OMIT_UTF16
        u_int8_t enc = ENC(db);
        if( enc!=DBSQL_UTF8 ){
          memset(&utf8, 0, sizeof(utf8));
          utf8.db = db;
          __vdbe_mem_set_str(&utf8, var->z, var->n, enc, DBSQL_STATIC);
          if( DBSQL_NOMEM==__vdbe_change_encoding(&utf8, DBSQL_UTF8) ){
            out.acc_error = DBSQL_NOMEM;
            out.n_alloc = 0;
          }
          var = &utf8;
        }
#endif
        n_out = var->n;
#ifdef DBSQL_TRACE_SIZE_LIMIT
        if( n_out>DBSQL_TRACE_SIZE_LIMIT ){
          n_out = DBSQL_TRACE_SIZE_LIMIT;
          while( n_out<var->n && (var->z[n_out]&0xc0)==0x80 ){ n_out++; }
        }
#endif    
        dbsql_str_appendf(&out, "'%.*q'", n_out, var->z);
#ifdef DBSQL_TRACE_SIZE_LIMIT
        if( n_out<var->n ){
          dbsql_str_appendf(&out, "/*+%d bytes*/", var->n-n_out);
        }
#endif
#ifndef DBSQL_OMIT_UTF16
        if( enc!=DBSQL_UTF8 ) __vdbe_mem_release(&utf8);
#endif
      }else if( var->flags & MEM_Zero ){
        dbsql_str_appendf(&out, "zeroblob(%d)", var->u.n_zero);
      }else{
        int n_out;  /* Number of bytes of the blob to include in output */
        assert( var->flags & MEM_Blob );
        dbsql_str_append(&out, "x'", 2);
        n_out = var->n;
#ifdef DBSQL_TRACE_SIZE_LIMIT
        if( n_out>DBSQL_TRACE_SIZE_LIMIT ) n_out = DBSQL_TRACE_SIZE_LIMIT;
#endif
        for(i=0; i<n_out; i++){
          dbsql_str_appendf(&out, "%02x", var->z[i]&0xff);
        }
        dbsql_str_append(&out, "'", 1);
#ifdef DBSQL_TRACE_SIZE_LIMIT
        if( n_out<var->n ){
          dbsql_str_appendf(&out, "/*+%d bytes*/", var->n-n_out);
        }
#endif
      }
    }
  }
  if( out.acc_error ) dbsql_str_reset(&out);
  return __str_accum_finish(&out);
}

#endif /* #ifndef DBSQL_OMIT_TRACE */
