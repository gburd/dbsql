/*
** 2025-10-20
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Implementation of the Query Result-Format or "qrf" utility library for
** dbsql.  See the README.md documentation for additional information.
*/
#ifndef DBSQL_QRF_H
#include "qrf.h"
#endif
#include <string.h>
#include <assert.h>
#include <stdint.h>

#ifndef DBSQL_AMALGAMATION
typedef int64_t int64_t;
#endif

/* A single line in the EQP output */
typedef struct __qrf_eqp_graph_row __qrf_eqp_graph_row;
struct __qrf_eqp_graph_row {
  int i_eqp_id;            /* ID for this row */
  int i_parent_id;         /* ID of the parent row */
  __qrf_eqp_graph_row *next; /* Next row in sequence */
  char text[1];         /* Text to display for this row */
};

/* All EQP output is collected into an instance of the following */
typedef struct qrf_eqp_graph qrf_eqp_graph;
struct qrf_eqp_graph {
  __qrf_eqp_graph_row *row;  /* Linked list of all rows of the EQP output */
  __qrf_eqp_graph_row *last; /* Last element of the row list */
  int n_width;            /* Width of the graph */
  char prefix[400];     /* Graph prefix */
};

/*
** Private state information.  Subject to change from one release to the
** next.
*/
typedef struct qrf_t qrf_t;
struct qrf_t {
  dbsql_stmt_t *stmt;        /* The statement whose output is to be rendered */
  DBSQL *db;                /* The corresponding database connection */
  dbsql_stmt_t *j_trans;      /* JSONB to JSON translator statement */
  char **err_p;               /* Write error message here, if not NULL */
  dbsql_str_t *out;          /* Accumulated output */
  int i_err;                   /* Error code */
  int n_col;                   /* Number of output columns */
  int exp_mode;                /* Original dbsql_stmt_isexplain() plus 1 */
  int mx_width;                /* Screen width */
  int mx_height;               /* n_line_limit */
  union {
    struct {                  /* Content for QRF_STYLE_Line */
      int mx_col_wth;             /* Maximum display width of any column */
      char **cols;             /* Names of output columns (MODE_Line) */
    } s_line;
    qrf_eqp_graph *graph;      /* EQP graph (Eqp, Stats, and __stats_est) */
    struct {                  /* Content for QRF_STYLE_Explain */
      int n_indent;              /* Slots allocated for indents */
      int i_indent;              /* Current slot */
      int *indents;            /* Indentation for each opcode */
    } s_expln;
    unsigned int n_ins;        /* Bytes used for current INSERT stmt */
  } u;
  int64_t n_row;         /* Number of rows handled so far */
  int *actual_width;           /* Actual width of each column */
  dbsql_qrf_spec spec;      /* Copy of the original spec */
};

/*
** Data for substitute ctype.h functions.  Used for x-platform
** consistency and so that '_' is counted as an alphabetic
** character.
**
**    0x01 -  space
**    0x02 -  digit
**    0x04 -  alphabetic, including '_'
*/
static const char __qrf_c_type[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0,
  0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 4,
  0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
#define QRF_SPACE(x) ((__qrf_c_type[(unsigned char)x]&1)!=0)
#define QRF_DIGIT(x) ((__qrf_c_type[(unsigned char)x]&2)!=0)
#define QRF_ALPHA(x) ((__qrf_c_type[(unsigned char)x]&4)!=0)
#define QRF_ALNUM(x) ((__qrf_c_type[(unsigned char)x]&6)!=0)

#ifndef deliberate_fall_through
/* Quiet some compilers about some of our intentional code. */
# if defined(GCC_VERSION) && GCC_VERSION>=7000000
#  define deliberate_fall_through __attribute__((fallthrough));
# else
#  define deliberate_fall_through
# endif
#endif

/*
** Set an error code and error message.
*/
static void __qrf_error(
  qrf_t *p,                /* Query result state */
  int i_code,             /* Error code */
  const char *format,   /* Message format (or NULL) */
  ...
){
  p->i_err = i_code;
  if( p->err_p!=0 ){
    dbsql_free(*p->err_p);
    *p->err_p = 0;
    if( format ){
      va_list ap;
      va_start(ap, format);
      *p->err_p = dbsql_vmprintf(format, ap);
      va_end(ap);
    }
  }
}

/*
** Out-of-memory error.
*/
static void __qrf_oom(qrf_t *p){
  __qrf_error(p, DBSQL_NOMEM, "out of memory");
}

/*
** Transfer any error in str over into p.
*/
static void __qrf_str_err(qrf_t *p, dbsql_str_t *str){
  int rc = str ? dbsql_str_errcode(str) : 0;
  if( rc ){
    __qrf_error(p, rc, dbsql_errstr(rc));
  }
}


/*
** Add a new entry to the EXPLAIN QUERY PLAN data
*/
static void __qrf_eqp_append(qrf_t *p, int i_eqp_id, int p2, const char *text){
  __qrf_eqp_graph_row *p_new;
  int64_t n_text;
  if( text==0 ) return;
  if( p->u.graph==0 ){
    p->u.graph = dbsql_malloc64( sizeof(qrf_eqp_graph) );
    if( p->u.graph==0 ){
      __qrf_oom(p);
      return;
    }
    memset(p->u.graph, 0, sizeof(qrf_eqp_graph) );
  }
  n_text = strlen(text);
  p_new = dbsql_malloc64( sizeof(*p_new) + n_text );
  if( p_new==0 ){
    __qrf_oom(p);
    return;
  }
  p_new->i_eqp_id = i_eqp_id;
  p_new->i_parent_id = p2;
  memcpy(p_new->text, text, n_text+1);
  p_new->next = 0;
  if( p->u.graph->last ){
    p->u.graph->last->next = p_new;
  }else{
    p->u.graph->row = p_new;
  }
  p->u.graph->last = p_new;
}

/*
** Free and reset the EXPLAIN QUERY PLAN data that has been collected
** in p->u.graph.
*/
static void __qrf_eqp_reset(qrf_t *p){
  __qrf_eqp_graph_row *row, *next;
  if( p->u.graph ){
    for(row = p->u.graph->row; row; row = next){
      next = row->next;
      dbsql_free(row);
    }
    dbsql_free(p->u.graph);
    p->u.graph = 0;
  }
}

/* Return the next EXPLAIN QUERY PLAN line with i_eqp_id that occurs after
** old, or return the first such line if old is NULL
*/
static __qrf_eqp_graph_row *__qrf_eqp_next_row(qrf_t *p, int i_eqp_id, __qrf_eqp_graph_row *old){
  __qrf_eqp_graph_row *row = old ? old->next : p->u.graph->row;
  while( row && row->i_parent_id!=i_eqp_id ) row = row->next;
  return row;
}

/* Render a single level of the graph that has i_eqp_id as its parent.  Called
** recursively to render sublevels.
*/
static void __qrf_eqp_render_level(qrf_t *p, int i_eqp_id){
  __qrf_eqp_graph_row *row, *next;
  int64_t n = strlen(p->u.graph->prefix);
  char *z;
  for(row = __qrf_eqp_next_row(p, i_eqp_id, 0); row; row = next){
    next = __qrf_eqp_next_row(p, i_eqp_id, row);
    z = row->text;
    dbsql_str_appendf(p->out, "%s%s%s\n", p->u.graph->prefix,
                            next ? "|--" : "`--", z);
    if( n<(int64_t)sizeof(p->u.graph->prefix)-7 ){
      memcpy(&p->u.graph->prefix[n], next ? "|  " : "   ", 4);
      __qrf_eqp_render_level(p, row->i_eqp_id);
      p->u.graph->prefix[n] = 0;
    }
  }
}

/*
** Render the 64-bit value N in a more human-readable format into
** out.
**
**   +  Only show the first three significant digits.
**   +  Append suffixes K, M, G, T, P, and E for 1e3, 1e6, ... 1e18
*/
static void __qrf_approx_int64(dbsql_str_t *out, int64_t N){
  static const char suffixes[] = { 'K', 'M', 'G', 'T', 'P', 'E' };
  int i;
  if( N<0 ){
    N = N==INT64_MIN ? INT64_MAX : -N;
    dbsql_str_append(out, "-", 1);
  }
  if( N<10000 ){
    dbsql_str_appendf(out, "%4lld ", N);
    return;
  }
  for(i=1; i<=18; i++){
    N = (N+5)/10;
    if( N<10000 ){
      int n = (int)N;
      switch( i%3 ){
        case 0:
          dbsql_str_appendf(out, "%d.%02d", n/1000, (n%1000)/10);
          break;
        case 1:
          dbsql_str_appendf(out, "%2d.%d", n/100, (n%100)/10);
          break;
        case 2:
          dbsql_str_appendf(out, "%4d", n/10);
          break;
      }
      dbsql_str_append(out, &suffixes[i/3], 1);
      break;
    }
  }
}

/*
** Display and reset the EXPLAIN QUERY PLAN data
*/
static void __qrf_eqp_render(qrf_t *p, int64_t n_cycle){
  __qrf_eqp_graph_row *row;
  if( p->u.graph!=0 && (row = p->u.graph->row)!=0 ){
    if( row->text[0]=='-' ){
      if( row->next==0 ){
        __qrf_eqp_reset(p);
        return;
      }
      dbsql_str_appendf(p->out, "%s\n", row->text+3);
      p->u.graph->row = row->next;
      dbsql_free(row);
    }else if( n_cycle>0 ){
      int n_sp = p->u.graph->n_width - 2;
      if( p->spec.style==QRF_STYLE_StatsEst ){
        dbsql_str_appendchar(p->out, n_sp, ' ');
        dbsql_str_appendall(p->out,
                                "Cycles      Loops  (est)  Rows   (est)\n");
        dbsql_str_appendchar(p->out, n_sp, ' ');
        dbsql_str_appendall(p->out,
                                "----------  ------------  ------------\n");
      }else{
        dbsql_str_appendchar(p->out, n_sp, ' ');
        dbsql_str_appendall(p->out,
                                "Cycles      Loops  Rows \n");
        dbsql_str_appendchar(p->out, n_sp, ' ');
        dbsql_str_appendall(p->out,
                                "----------  -----  -----\n");
      }
      dbsql_str_appendall(p->out, "QUERY PLAN");
      dbsql_str_appendchar(p->out, n_sp - 10, ' ');
      __qrf_approx_int64(p->out, n_cycle);
      dbsql_str_appendall(p->out, " 100%\n");
    }else{
      dbsql_str_appendall(p->out, "QUERY PLAN\n");
    }
    p->u.graph->prefix[0] = 0;
    __qrf_eqp_render_level(p, 0);
    __qrf_eqp_reset(p);
  }
}

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
/*
** Helper function for qrf_exp_stats().
**
*/
static int __qrf_stats_height(dbsql_stmt_t *p, int i_entry){
  int i_pid = 0;
  int ret = 1;
  dbsql_stmt_scanstatus_v2(p, i_entry,
      DBSQL_SCANSTAT_SELECTID, DBSQL_SCANSTAT_COMPLEX, (void*)&i_pid
  );
  while( i_pid!=0 ){
    int ii;
    for(ii=0; 1; ii++){
      int i_id;
      int res;
      res = dbsql_stmt_scanstatus_v2(p, ii,
          DBSQL_SCANSTAT_SELECTID, DBSQL_SCANSTAT_COMPLEX, (void*)&i_id
      );
      if( res ) break;
      if( i_id==i_pid ){
        dbsql_stmt_scanstatus_v2(p, ii,
            DBSQL_SCANSTAT_PARENTID, DBSQL_SCANSTAT_COMPLEX, (void*)&i_pid
        );
      }
    }
    ret++;
  }
  return ret;
}
#endif /* DBSQL_ENABLE_STMT_SCANSTATUS */


/*
** Generate ".scanstatus est" style of EQP output.
*/
static void __qrf_eqp_stats(qrf_t *p){
#ifndef DBSQL_ENABLE_STMT_SCANSTATUS
  __qrf_error(p, DBSQL_ERROR, "not available in this build");
#else
  static const int f = DBSQL_SCANSTAT_COMPLEX;
  dbsql_stmt_t *s = p->stmt;
  int i = 0;
  int64_t n_total = 0;
  int n_width = 0;
  int prev_pid = -1;             /* Previous i_pid */
  double r_est_cum = 1.0;         /* Cumulative row estimate */
  dbsql_str_t *line = dbsql_str_new(p->db);
  dbsql_str_t *stats = dbsql_str_new(p->db);
  __qrf_eqp_reset(p);

  for(i=0; 1; i++){
    const char *z = 0;
    int n = 0;
    if( dbsql_stmt_scanstatus_v2(s,i,DBSQL_SCANSTAT_EXPLAIN,f,(void*)&z) ){
      break;
    }
    n = (int)strlen(z) + __qrf_stats_height(s,i)*3;
    if( n>n_width ) n_width = n;
  }
  n_width += 2;

  dbsql_stmt_scanstatus_v2(s,-1, DBSQL_SCANSTAT_NCYCLE, f, (void*)&n_total);
  for(i=0; 1; i++){
    int64_t n_loop = 0;
    int64_t n_row = 0;
    int64_t n_cycle = 0;
    int i_id = 0;
    int i_pid = 0;
    const char *zo = 0;
    const char *name = 0;
    double r_est = 0.0;

    if( dbsql_stmt_scanstatus_v2(s,i,DBSQL_SCANSTAT_EXPLAIN,f,(void*)&zo) ){
      break;
    }
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_PARENTID,f,(void*)&i_pid);
    if( i_pid!=prev_pid ){
      prev_pid = i_pid;
      r_est_cum = 1.0;
    }
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_EST,f,(void*)&r_est);
    r_est_cum *= r_est;
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_NLOOP,f,(void*)&n_loop);
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_NVISIT,f,(void*)&n_row);
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_NCYCLE,f,(void*)&n_cycle);
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_SELECTID,f,(void*)&i_id);
    dbsql_stmt_scanstatus_v2(s,i, DBSQL_SCANSTAT_NAME,f,(void*)&name);

    if( n_cycle>=0 || n_loop>=0 || n_row>=0 ){
      int n_sp = 0;
      dbsql_str_reset(stats);
      if( n_cycle>=0 && n_total>0 ){
        __qrf_approx_int64(stats, n_cycle);
        dbsql_str_appendf(stats, " %3d%%",
            ((n_cycle*100)+n_total/2) / n_total
        );
        n_sp = 2;
      }
      if( n_loop>=0 ){
        if( n_sp ) dbsql_str_appendchar(stats, n_sp, ' ');
        __qrf_approx_int64(stats, n_loop);
        n_sp = 2;
        if( p->spec.style==QRF_STYLE_StatsEst ){
          dbsql_str_appendf(stats, "  ");
          __qrf_approx_int64(stats, (int64_t)(r_est_cum/r_est));
        }
      }
      if( n_row>=0 ){
        if( n_sp ) dbsql_str_appendchar(stats, n_sp, ' ');
        __qrf_approx_int64(stats, n_row);
        n_sp = 2;
        if( p->spec.style==QRF_STYLE_StatsEst ){
          dbsql_str_appendf(stats, "  ");
          __qrf_approx_int64(stats, (int64_t)r_est_cum);
        }
      }
      dbsql_str_appendf(line,
          "% *s %s", -1*(n_width-__qrf_stats_height(s,i)*3), zo,
          dbsql_str_value(stats)
      );
      dbsql_str_reset(stats);
      __qrf_eqp_append(p, i_id, i_pid, dbsql_str_value(line));
      dbsql_str_reset(line);
    }else{
      __qrf_eqp_append(p, i_id, i_pid, zo);
    }
  }
  if( p->u.graph ) p->u.graph->n_width = n_width;
  __qrf_str_err(p, line);
  dbsql_free(dbsql_str_finish(line));
  __qrf_str_err(p, stats);
  dbsql_free(dbsql_str_finish(stats));
#endif
}


/*
** Reset the prepared statement.
*/
static void __qrf_reset_stmt(qrf_t *p){
  int rc = dbsql_reset(p->stmt);
  if( rc!=DBSQL_OK && p->i_err==DBSQL_OK ){
    __qrf_error(p, rc, "%s", dbsql_errmsg(p->db));
  }
}

/*
** If x_write is defined, send all content of out to x_write and
** reset out.
*/
static void __qrf_write(qrf_t *p){
  int n;
  if( p->spec.x_write && (n = dbsql_str_length(p->out))>0 ){
    int rc = p->spec.x_write(p->spec.write_arg,
                 dbsql_str_value(p->out),
                 (int64_t)n);
    dbsql_str_reset(p->out);
    if( rc ){
      __qrf_error(p, rc, "Failed to write %d bytes of output", n);
    }
  }
}

/* Lookup table to estimate the number of columns consumed by a Unicode
** character.
*/
static const struct {
  unsigned char w;    /* Width of the character in columns */
  int i_first;         /* First character in a span having this width */
} __a_qrf_u_width[] = {
   /* {1, 0x00000}, */
  {0, 0x00300},  {1, 0x00370},  {0, 0x00483},  {1, 0x00487},  {0, 0x00488},
  {1, 0x0048a},  {0, 0x00591},  {1, 0x005be},  {0, 0x005bf},  {1, 0x005c0},
  {0, 0x005c1},  {1, 0x005c3},  {0, 0x005c4},  {1, 0x005c6},  {0, 0x005c7},
  {1, 0x005c8},  {0, 0x00600},  {1, 0x00604},  {0, 0x00610},  {1, 0x00616},
  {0, 0x0064b},  {1, 0x0065f},  {0, 0x00670},  {1, 0x00671},  {0, 0x006d6},
  {1, 0x006e5},  {0, 0x006e7},  {1, 0x006e9},  {0, 0x006ea},  {1, 0x006ee},
  {0, 0x0070f},  {1, 0x00710},  {0, 0x00711},  {1, 0x00712},  {0, 0x00730},
  {1, 0x0074b},  {0, 0x007a6},  {1, 0x007b1},  {0, 0x007eb},  {1, 0x007f4},
  {0, 0x00901},  {1, 0x00903},  {0, 0x0093c},  {1, 0x0093d},  {0, 0x00941},
  {1, 0x00949},  {0, 0x0094d},  {1, 0x0094e},  {0, 0x00951},  {1, 0x00955},
  {0, 0x00962},  {1, 0x00964},  {0, 0x00981},  {1, 0x00982},  {0, 0x009bc},
  {1, 0x009bd},  {0, 0x009c1},  {1, 0x009c5},  {0, 0x009cd},  {1, 0x009ce},
  {0, 0x009e2},  {1, 0x009e4},  {0, 0x00a01},  {1, 0x00a03},  {0, 0x00a3c},
  {1, 0x00a3d},  {0, 0x00a41},  {1, 0x00a43},  {0, 0x00a47},  {1, 0x00a49},
  {0, 0x00a4b},  {1, 0x00a4e},  {0, 0x00a70},  {1, 0x00a72},  {0, 0x00a81},
  {1, 0x00a83},  {0, 0x00abc},  {1, 0x00abd},  {0, 0x00ac1},  {1, 0x00ac6},
  {0, 0x00ac7},  {1, 0x00ac9},  {0, 0x00acd},  {1, 0x00ace},  {0, 0x00ae2},
  {1, 0x00ae4},  {0, 0x00b01},  {1, 0x00b02},  {0, 0x00b3c},  {1, 0x00b3d},
  {0, 0x00b3f},  {1, 0x00b40},  {0, 0x00b41},  {1, 0x00b44},  {0, 0x00b4d},
  {1, 0x00b4e},  {0, 0x00b56},  {1, 0x00b57},  {0, 0x00b82},  {1, 0x00b83},
  {0, 0x00bc0},  {1, 0x00bc1},  {0, 0x00bcd},  {1, 0x00bce},  {0, 0x00c3e},
  {1, 0x00c41},  {0, 0x00c46},  {1, 0x00c49},  {0, 0x00c4a},  {1, 0x00c4e},
  {0, 0x00c55},  {1, 0x00c57},  {0, 0x00cbc},  {1, 0x00cbd},  {0, 0x00cbf},
  {1, 0x00cc0},  {0, 0x00cc6},  {1, 0x00cc7},  {0, 0x00ccc},  {1, 0x00cce},
  {0, 0x00ce2},  {1, 0x00ce4},  {0, 0x00d41},  {1, 0x00d44},  {0, 0x00d4d},
  {1, 0x00d4e},  {0, 0x00dca},  {1, 0x00dcb},  {0, 0x00dd2},  {1, 0x00dd5},
  {0, 0x00dd6},  {1, 0x00dd7},  {0, 0x00e31},  {1, 0x00e32},  {0, 0x00e34},
  {1, 0x00e3b},  {0, 0x00e47},  {1, 0x00e4f},  {0, 0x00eb1},  {1, 0x00eb2},
  {0, 0x00eb4},  {1, 0x00eba},  {0, 0x00ebb},  {1, 0x00ebd},  {0, 0x00ec8},
  {1, 0x00ece},  {0, 0x00f18},  {1, 0x00f1a},  {0, 0x00f35},  {1, 0x00f36},
  {0, 0x00f37},  {1, 0x00f38},  {0, 0x00f39},  {1, 0x00f3a},  {0, 0x00f71},
  {1, 0x00f7f},  {0, 0x00f80},  {1, 0x00f85},  {0, 0x00f86},  {1, 0x00f88},
  {0, 0x00f90},  {1, 0x00f98},  {0, 0x00f99},  {1, 0x00fbd},  {0, 0x00fc6},
  {1, 0x00fc7},  {0, 0x0102d},  {1, 0x01031},  {0, 0x01032},  {1, 0x01033},
  {0, 0x01036},  {1, 0x0103b},  {0, 0x01058},
  {1, 0x0105a},  {2, 0x01100},  {0, 0x01160},  {1, 0x01200},  {0, 0x0135f},
  {1, 0x01360},  {0, 0x01712},  {1, 0x01715},  {0, 0x01732},  {1, 0x01735},
  {0, 0x01752},  {1, 0x01754},  {0, 0x01772},  {1, 0x01774},  {0, 0x017b4},
  {1, 0x017b6},  {0, 0x017b7},  {1, 0x017be},  {0, 0x017c6},  {1, 0x017c7},
  {0, 0x017c9},  {1, 0x017d4},  {0, 0x017dd},  {1, 0x017de},  {0, 0x0180b},
  {1, 0x0180e},  {0, 0x018a9},  {1, 0x018aa},  {0, 0x01920},  {1, 0x01923},
  {0, 0x01927},  {1, 0x01929},  {0, 0x01932},  {1, 0x01933},  {0, 0x01939},
  {1, 0x0193c},  {0, 0x01a17},  {1, 0x01a19},  {0, 0x01b00},  {1, 0x01b04},
  {0, 0x01b34},  {1, 0x01b35},  {0, 0x01b36},  {1, 0x01b3b},  {0, 0x01b3c},
  {1, 0x01b3d},  {0, 0x01b42},  {1, 0x01b43},  {0, 0x01b6b},  {1, 0x01b74},
  {0, 0x01dc0},  {1, 0x01dcb},  {0, 0x01dfe},  {1, 0x01e00},  {0, 0x0200b},
  {1, 0x02010},  {0, 0x0202a},  {1, 0x0202f},  {0, 0x02060},  {1, 0x02064},
  {0, 0x0206a},  {1, 0x02070},  {0, 0x020d0},  {1, 0x020f0},  {2, 0x02329},
  {1, 0x0232b},  {2, 0x02e80},  {0, 0x0302a},  {2, 0x03030},  {1, 0x0303f},
  {2, 0x03040},  {0, 0x03099},  {2, 0x0309b},  {1, 0x0a4d0},  {0, 0x0a806},
  {1, 0x0a807},  {0, 0x0a80b},  {1, 0x0a80c},  {0, 0x0a825},  {1, 0x0a827},
  {2, 0x0ac00},  {1, 0x0d7a4},  {2, 0x0f900},  {1, 0x0fb00},  {0, 0x0fb1e},
  {1, 0x0fb1f},  {0, 0x0fe00},  {2, 0x0fe10},  {1, 0x0fe1a},  {0, 0x0fe20},
  {1, 0x0fe24},  {2, 0x0fe30},  {1, 0x0fe70},  {0, 0x0feff},  {2, 0x0ff00},
  {1, 0x0ff61},  {2, 0x0ffe0},  {1, 0x0ffe7},  {0, 0x0fff9},  {1, 0x0fffc},
  {0, 0x10a01},  {1, 0x10a04},  {0, 0x10a05},  {1, 0x10a07},  {0, 0x10a0c},
  {1, 0x10a10},  {0, 0x10a38},  {1, 0x10a3b},  {0, 0x10a3f},  {1, 0x10a40},
  {0, 0x1d167},  {1, 0x1d16a},  {0, 0x1d173},  {1, 0x1d183},  {0, 0x1d185},
  {1, 0x1d18c},  {0, 0x1d1aa},  {1, 0x1d1ae},  {0, 0x1d242},  {1, 0x1d245},
  {2, 0x20000},  {1, 0x2fffe},  {2, 0x30000},  {1, 0x3fffe},  {0, 0xe0001},
  {1, 0xe0002},  {0, 0xe0020},  {1, 0xe0080},  {0, 0xe0100},  {1, 0xe01f0}
};

/*
** Return an estimate of the width, in columns, for the single Unicode
** character c.  For normal characters, the answer is always 1.  But the
** estimate might be 0 or 2 for zero-width and double-width characters.
**
** Different display devices display unicode using different widths.  So
** it is impossible to know that true display width with 100% accuracy.
** Inaccuracies in the width estimates might cause columns to be misaligned.
** Unfortunately, there is nothing we can do about that.
*/
int dbsql_qrf_wcwidth(int c){
  int i_first, i_last;

  /* Fast path for common characters */
  if( c<0x300 ) return 1;

  /* The general case */
  i_first = 0;
  i_last = sizeof(__a_qrf_u_width)/sizeof(__a_qrf_u_width[0]) - 1;
  while( i_first<i_last-1 ){
    int i_mid = (i_first+i_last)/2;
    int c_mid = __a_qrf_u_width[i_mid].i_first;
    if( c_mid < c ){
      i_first = i_mid;
    }else if( c_mid > c ){
      i_last = i_mid - 1;
    }else{
      return __a_qrf_u_width[i_mid].w;
    }
  }
  if( __a_qrf_u_width[i_last].i_first > c ) return __a_qrf_u_width[i_first].w;
  return __a_qrf_u_width[i_last].w;
}

/*
** Compute the value and length of a multi-byte UTF-8 character that
** begins at z[0]. Return the length.  Write the Unicode value into *u.
**
** This routine only works for *multi-byte* UTF-8 characters.  It does
** not attempt to detect illegal characters.
*/
int dbsql_qrf_decode_utf8(const unsigned char *z, int *u){
  if( (z[0] & 0xe0)==0xc0 && (z[1] & 0xc0)==0x80 ){
    *u = ((z[0] & 0x1f)<<6) | (z[1] & 0x3f);
    return 2;
  }
  if( (z[0] & 0xf0)==0xe0 && (z[1] & 0xc0)==0x80 && (z[2] & 0xc0)==0x80 ){
    *u = ((z[0] & 0x0f)<<12) | ((z[1] & 0x3f)<<6) | (z[2] & 0x3f);
    return 3;
  }
  if( (z[0] & 0xf8)==0xf0 && (z[1] & 0xc0)==0x80 && (z[2] & 0xc0)==0x80
   && (z[3] & 0xc0)==0x80
  ){
    *u = ((z[0] & 0x0f)<<18) | ((z[1] & 0x3f)<<12) | ((z[2] & 0x3f))<<6
                              | (z[3] & 0x3f);
    return 4;
  }
  *u = 0;
  return 1;
}

/*
** Check to see if z[] is a valid VT100 escape.  If it is, then
** return the number of bytes in the escape sequence.  Return 0 if
** z[] is not a VT100 escape.
**
** This routine assumes that z[0] is \033 (ESC).
*/
static int __qrf_is_vt100(const unsigned char *z){
  int i;
  if( z[1]!='[' ) return 0;
  i = 2;
  while( z[i]>=0x30 && z[i]<=0x3f ){ i++; }
  while( z[i]>=0x20 && z[i]<=0x2f ){ i++; }
  if( z[i]<0x40 || z[i]>0x7e ) return 0;
  return i+1;
}

/*
** Return the length of a string in display characters.
**
** Most characters of the input string count as 1, including
** multi-byte UTF8 characters.  However, zero-width unicode
** characters and VT100 escape sequences count as zero, and
** double-width characters count as two.
**
** The definition of "zero-width" and "double-width" characters
** is not precise.  It depends on the output device, to some extent,
** and it varies according to the Unicode version.  This routine
** makes the best guess that it can.
*/
size_t dbsql_qrf_wcswidth(const char *in){
  const unsigned char *z = (const unsigned char*)in;
  size_t n = 0;
  while( *z ){
    if( z[0]<' ' ){
      int k;
      if( z[0]=='\033' && (k = __qrf_is_vt100(z))>0 ){
        z += k;
      }else{
        z++;
      }
    }else if( (0x80&z[0])==0 ){
      n++;
      z++;
    }else{
      int u = 0;
      int len = dbsql_qrf_decode_utf8(z, &u);
      z += len;
      n += dbsql_qrf_wcwidth(u);
    }
  }
  return n;
}

/*
** Return the display width of the longest line of text
** in the (possibly) multi-line input string in[0..n_byte].
** in[] is not necessarily zero-terminated.  Take
** into account tab characters, zero- and double-width
** characters, CR and NL, and VT100 escape codes.
**
** Write the number of newlines into *pn_nl.  So, *pn_nl will
** return 0 if everything fits on one line, or positive it
** it will need to be split.
*/
static int __qrf_display_width(const char *in, int64_t n_byte, int *pn_nl){
  const unsigned char *z;
  const unsigned char *end;
  int mx = 0;
  int n = 0;
  int n_nl = 0;
  if( in==0 ) in = "";
  z = (const unsigned char*)in;
  end = &z[n_byte];
  while( z<end ){
    if( z[0]<' ' ){
      int k;
      if( z[0]=='\033' && (k = __qrf_is_vt100(z))>0 ){
        z += k;
      }else{
        if( z[0]=='\t' ){
          n = (n+8)&~7;
        }else if( z[0]=='\n' || z[0]=='\r' ){
          n_nl++;
          if( n>mx ) mx = n;
          n = 0;
        }
        z++;
      }
    }else if( (0x80&z[0])==0 ){
      n++;
      z++;
    }else{
      int u = 0;
      int len = dbsql_qrf_decode_utf8(z, &u);
      z += len;
      n += dbsql_qrf_wcwidth(u);
    }
  }
  if( mx>n ) n = mx;
  if( pn_nl ) *pn_nl = n_nl;
  return n;
}

/*
** Escape the input string if it is needed and in accordance with
** esc, which is either QRF_ESC_Ascii or QRF_ESC_Symbol.
**
** Escaping is needed if the string contains any control characters
** other than \t, \n, and \r\n
**
** If no escaping is needed (the common case) then set *out to NULL
** and return 0.  If escaping is needed, write the escaped string into
** memory obtained from dbsql_malloc64() and make *out point to that
** memory and return 0.  If an error occurs, return non-zero.
**
** The caller is responsible for freeing *pp_free if it is non-NULL in order
** to reclaim memory.
*/
static void __qrf_escape(
  int esc,            /* QRF_ESC_Ascii or QRF_ESC_Symbol */
  dbsql_str_t *str,      /* String to be escaped */
  int i_start              /* Begin escapding on this byte of str */
){
  int64_t i, j;     /* Loop counters */
  int64_t sz;       /* Size of the string prior to escaping */
  int64_t n_ctrl = 0;/* Number of control characters to escape */
  unsigned char *in;     /* Text to be escaped */
  unsigned char c;        /* A single character of the text */
  unsigned char *out;    /* Where to write the results */

  /* Find the text to be escaped */
  in = (unsigned char*)dbsql_str_value(str);
  if( in==0 ) return;
  in += i_start;

  /* Count the control characters */
  for(i=0; (c = in[i])!=0; i++){
    if( c<=0x1f
     && c!='\t'
     && c!='\n'
     && (c!='\r' || in[i+1]!='\n')
    ){
      n_ctrl++;
    }
  }
  if( n_ctrl==0 ) return;  /* Early out if no control characters */

  /* Make space to hold the escapes.  Copy the original text to the end
  ** of the available space. */
  sz = dbsql_str_length(str) - i_start;
  if( esc==QRF_ESC_Symbol ) n_ctrl *= 2;
  dbsql_str_appendchar(str, n_ctrl, ' ');
  out = (unsigned char*)dbsql_str_value(str);
  if( out==0 ) return;
  out += i_start;
  in = out + n_ctrl;
  memmove(in,out,sz);

  /* Convert the control characters */
  for(i=j=0; (c = in[i])!=0; i++){
    if( c>0x1f
     || c=='\t'
     || c=='\n'
     || (c=='\r' && in[i+1]=='\n')
    ){
      continue;
    }
    if( i>0 ){
      memmove(&out[j], in, i);
      j += i;
    }
    in += i+1;
    i = -1;
    if( esc==QRF_ESC_Symbol ){
      out[j++] = 0xe2;
      out[j++] = 0x90;
      out[j++] = 0x80+c;
    }else{
      out[j++] = '^';
      out[j++] = 0x40+c;
    }
  }
}

/*
** Determine if the string z[] can be shown as plain text.  Return true
** if z[] is unambiguously text.  Return false if z[] needs to be
** quoted.
**
** All of the following must be true in order for z[] to be relaxable:
**
**    (1) z[] does not begin or end with ' or whitespace
**    (2) z[] is not the same as the NULL rendering
**    (3) z[] does not looks like a numeric literal
*/
static int __qrf_relaxable(qrf_t *p, const char *z){
  size_t i, n;
  if( z[0]=='\'' || QRF_SPACE(z[0]) ) return 0;
  if( z[0]==0 ){
    return (p->spec.null!=0 && p->spec.null[0]!=0);
  }
  n = strlen(z);
  if( n==0 || z[n-1]=='\'' || QRF_SPACE(z[n-1]) ) return 0;
  if( p->spec.null && strcmp(p->spec.null,z)==0 ) return 0;
  i = (z[0]=='-' || z[0]=='+');
  if( strcmp(z+i,"Inf")==0 ) return 0;
  if( !QRF_DIGIT(z[i]) ) return 1;
  i++;
  while( QRF_DIGIT(z[i]) ){ i++; }
  if( z[i]==0 ) return 0;
  if( z[i]=='.' ){
    i++;
    while( QRF_DIGIT(z[i]) ){ i++; }
    if( z[i]==0 ) return 0;
  }
  if( z[i]=='e' || z[i]=='E' ){
    i++;
    if( z[i]=='+' || z[i]=='-' ){ i++; }
    if( !QRF_DIGIT(z[i]) ) return 1;
    i++;
    while( QRF_DIGIT(z[i]) ){ i++; }
  }
  return z[i]!=0;
}

/*
** If a field contains any character identified by a 1 in the following
** array, then the string must be quoted for CSV.
*/
static const char __qrf_csv_quote[] = {
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 0, 1, 0, 0, 0, 0, 1,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
};

/*
** Encode text appropriately and append it to out.
*/
static void __qrf_encode_text(qrf_t *p, dbsql_str_t *out, const char *txt){
  int i_start = dbsql_str_length(out);
  switch( p->spec.text ){
    case QRF_TEXT_Relaxed:
      if( __qrf_relaxable(p, txt) ){
        dbsql_str_appendall(out, txt);
        break;
      }
      deliberate_fall_through; /* FALLTHRU */
    case QRF_TEXT_Sql: {
      if( p->spec.esc==QRF_ESC_Off ){
        dbsql_str_appendf(out, "%Q", txt);
      }else{
        dbsql_str_appendf(out, "%#Q", txt);
      }
      break;
    }
    case QRF_TEXT_Csv: {
      unsigned int i;
      for(i=0; txt[i]; i++){
        if( __qrf_csv_quote[((const unsigned char*)txt)[i]] ){
          i = 0;
          break;
        }
      }
      if( i==0 || strstr(txt, p->spec.column_sep)!=0 ){
        dbsql_str_appendf(out, "\"%w\"", txt);
      }else{
        dbsql_str_appendall(out, txt);
      }
      break;
    }
    case QRF_TEXT_Html: {
      const unsigned char *z = (const unsigned char*)txt;
      while( *z ){
        unsigned int i = 0;
        unsigned char c;
        while( (c=z[i])>'>'
            || (c && c!='<' && c!='>' && c!='&' && c!='\"' && c!='\'')
        ){
          i++;
        }
        if( i>0 ){
          dbsql_str_append(out, (const char*)z, i);
        }
        switch( z[i] ){
          case '>':   dbsql_str_append(out, "&gt;", 4);   break;
          case '&':   dbsql_str_append(out, "&amp;", 5);  break;
          case '<':   dbsql_str_append(out, "&lt;", 4);   break;
          case '"':   dbsql_str_append(out, "&quot;", 6); break;
          case '\'':  dbsql_str_append(out, "&#39;", 5);  break;
          default:    i--;
        }
        z += i + 1;
      }
      break;
    }
    case QRF_TEXT_Tcl:
    case QRF_TEXT_Json: {
      const unsigned char *z = (const unsigned char*)txt;
      dbsql_str_append(out, "\"", 1);
      while( *z ){
        unsigned int i;
        for(i=0; z[i]>=0x20 && z[i]!='\\' && z[i]!='"'; i++){}
        if( i>0 ){
          dbsql_str_append(out, (const char*)z, i);
        }
        if( z[i]==0 ) break;
        switch( z[i] ){
          case '"':   dbsql_str_append(out, "\\\"", 2);  break;
          case '\\':  dbsql_str_append(out, "\\\\", 2);  break;
          case '\b':  dbsql_str_append(out, "\\b", 2);   break;
          case '\f':  dbsql_str_append(out, "\\f", 2);   break;
          case '\n':  dbsql_str_append(out, "\\n", 2);   break;
          case '\r':  dbsql_str_append(out, "\\r", 2);   break;
          case '\t':  dbsql_str_append(out, "\\t", 2);   break;
          default: {
            if( p->spec.text==QRF_TEXT_Json ){
              dbsql_str_appendf(out, "\\u%04x", z[i]);
            }else{
              dbsql_str_appendf(out, "\\%03o", z[i]);
            }
            break;
          }
        }
        z += i + 1;
      }
      dbsql_str_append(out, "\"", 1);
      break;
    }
    default: {
      dbsql_str_appendall(out, txt);
      break;
    }
  }
  if( p->spec.esc!=QRF_ESC_Off ){
    __qrf_escape(p->spec.esc, out, i_start);
  }
}

/*
** Do a quick sanity check to see blobs[0..n_blob-1] is valid JSONB
** return true if it is and false if it is not.
**
** False positives are possible, but not false negatives.
*/
static int __qrf_jsonb_quick_check(unsigned char *blobs, int n_blob){
  unsigned char x;   /* Payload size half-byte */
  int i;             /* Loop counter */   
  int n;             /* Bytes in the payload size integer */
  u_int64_t sz; /* value of the payload size integer */

  if( n_blob==0 ) return 0;
  x = blobs[0]>>4;
  if( x<=11 ) return n_blob==(1+x);
  n = x<14 ? x-11 : 4*(x-13);
  if( n_blob<1+n ) return 0;
  sz = blobs[1];
  for(i=1; i<n; i++) sz = (sz<<8) + blobs[i+1];
  return sz+n+1==(u_int64_t)n_blob;
}

/*
** The current i_col-th column of p->stmt is known to be a BLOB.  Check
** to see if that BLOB is really a JSONB blob.  If it is, then translate
** it into a text JSON representation and return a pointer to that text JSON.
** If the BLOB is not JSONB, then return a NULL pointer.
**
** The memory used to hold the JSON text is managed internally by the
** "p" object and is overwritten and/or deallocated upon the next call
** to this routine (with the same p argument) or when the p object is
** finailized.
*/
static const char *__qrf_jsonb_to_json(qrf_t *p, int i_col){
  int n_byte;
  const void *blob;
  int rc;
  n_byte = dbsql_column_bytes(p->stmt, i_col);
  blob = dbsql_column_blob(p->stmt, i_col);
  if( __qrf_jsonb_quick_check((unsigned char*)blob, n_byte)==0 ){
    return 0;
  }
  if( p->j_trans==0 ){
    DBSQL *db;
    rc = dbsql_open(":memory:",&db);
    if( rc ){
      dbsql_close(db);
      return 0;
    }
    rc = dbsql_prepare_v2(db, "SELECT json(?1)", -1, &p->j_trans, 0);
    if( rc ){
      dbsql_finalize(p->j_trans);
      p->j_trans = 0;
      dbsql_close(db);
      return 0;
    }
  }else{
    dbsql_reset(p->j_trans);
  }
  dbsql_bind_blob(p->j_trans, 1, (void*)blob, n_byte, DBSQL_STATIC);
  rc = dbsql_step(p->j_trans);
  if( rc==DBSQL_ROW ){
    return (const char*)dbsql_column_text(p->j_trans, 0);
  }else{
    return 0;
  }
}

/*
** Adjust the input string in[] such that it is no more than N display
** characters wide.  If it is wider than that, then truncate and add
** ellipsis.  Or if in[] contains a \r or \n, truncate at that point,
** adding ellipsis.  Embedded tabs in in[] are converted into ordinary
** spaces.
**
** Return this display width of the modified title string.
*/
static int __qrf_title_limit(char *in, int N){
  unsigned char *z = (unsigned char*)in;
  int n = 0;
  unsigned char *ellipsis = 0;
  while( 1 /*exit-by-break*/ ){
    if( z[0]<' ' ){
      int k;
      if( z[0]==0 ){
        ellipsis = 0;
        break;
      }else if( z[0]=='\033' && (k = __qrf_is_vt100(z))>0 ){
        z += k;
      }else if( z[0]=='\t' ){
        z[0] = ' ';
      }else if( z[0]=='\n' || z[0]=='\r' ){
        z[0] = ' ';
      }else{
        z++;
      }
    }else if( (0x80&z[0])==0 ){
      if( n>=(N-3) && ellipsis==0 ) ellipsis = z;
      if( n==N ){ z[0] = 0; break; }
      n++;
      z++;
    }else{
      int u = 0;
      int len = dbsql_qrf_decode_utf8(z, &u);
      if( n+len>(N-3) && ellipsis==0 ) ellipsis = z;
      if( n+len>N ){ z[0] = 0; break; }
      z += len;
      n += dbsql_qrf_wcwidth(u);
    }
  }
  if( ellipsis && N>=3 ) memcpy(ellipsis,"...",4);
  return n;
}


/*
** Render value val into out
*/
static void __qrf_render_value(qrf_t *p, dbsql_str_t *out, int i_col){
#if DBSQL_VERSION_NUMBER>=3052000
  int i_start_len = dbsql_str_length(out);
#endif
  if( p->spec.render ){
    dbsql_value_t *val;
    char *z;
    val = dbsql_value_dup(dbsql_column_value(p->stmt,i_col));
    z = p->spec.render(p->spec.render_arg, val);
    dbsql_value_free(val);
    if( z ){
      dbsql_str_appendall(out, z);
      dbsql_free(z);
      return;
    }
  }
  switch( dbsql_column_type(p->stmt,i_col) ){
    case DBSQL_INTEGER: {
      dbsql_str_appendf(out, "%lld", dbsql_column_int64(p->stmt,i_col));
      break;
    }
    case DBSQL_FLOAT: {
      const char *txt = (const char*)dbsql_column_text(p->stmt,i_col);
      dbsql_str_appendall(out, txt);
      break;
    }
    case DBSQL_BLOB: {
      if( p->spec.text_jsonb==QRF_Yes ){
        const char *json = __qrf_jsonb_to_json(p, i_col);
        if( json ){
          if( p->spec.text==QRF_TEXT_Sql ){
            dbsql_str_append(out,"jsonb(",6);
            __qrf_encode_text(p, out, json);
            dbsql_str_append(out,")",1);
          }else{
            __qrf_encode_text(p, out, json);
          }
          break;
        }
      }
      switch( p->spec.blob ){
        case QRF_BLOB_Hex:
        case QRF_BLOB_Sql: {
          int i_start;
          int n_blob = dbsql_column_bytes(p->stmt,i_col);
          int i, j;
          char *z_val;
          const unsigned char *a = dbsql_column_blob(p->stmt,i_col);
          if( p->spec.blob==QRF_BLOB_Sql ){
            dbsql_str_append(out, "x'", 2);
          }
          i_start = dbsql_str_length(out);
          dbsql_str_appendchar(out, n_blob, ' ');
          dbsql_str_appendchar(out, n_blob, ' ');
          if( p->spec.blob==QRF_BLOB_Sql ){
            dbsql_str_appendchar(out, 1, '\'');
          }
          if( dbsql_str_errcode(out) ) return;
          z_val = dbsql_str_value(out);
          for(i=0, j=i_start; i<n_blob; i++, j+=2){
            unsigned char c = a[i];
            z_val[j] = "0123456789abcdef"[(c>>4)&0xf];
            z_val[j+1] = "0123456789abcdef"[(c)&0xf];
          }
          break;
        }
        case QRF_BLOB_Tcl:
        case QRF_BLOB_Json: {
          int i_start;
          int n_blob = dbsql_column_bytes(p->stmt,i_col);
          int i, j;
          char *z_val;
          const unsigned char *a = dbsql_column_blob(p->stmt,i_col);
          int sz_c = p->spec.blob==QRF_BLOB_Json ? 6 : 4;
          dbsql_str_append(out, "\"", 1);
          i_start = dbsql_str_length(out);
          for(i=sz_c; i>0; i--){
            dbsql_str_appendchar(out, n_blob, ' ');
          }
          dbsql_str_appendchar(out, 1, '"');
          if( dbsql_str_errcode(out) ) return;
          z_val = dbsql_str_value(out);
          for(i=0, j=i_start; i<n_blob; i++, j+=sz_c){
            unsigned char c = a[i];
            z_val[j] = '\\';
            if( sz_c==4 ){
              z_val[j+1] = '0' + ((c>>6)&3);
              z_val[j+2] = '0' + ((c>>3)&7);
              z_val[j+3] = '0' + (c&7);
            }else{
              z_val[j+1] = 'u';
              z_val[j+2] = '0';
              z_val[j+3] = '0';
              z_val[j+4] = "0123456789abcdef"[(c>>4)&0xf];
              z_val[j+5] = "0123456789abcdef"[(c)&0xf];
            }
          }
          break;
        }
        case QRF_BLOB_Size: {
          int n_blob = dbsql_column_bytes(p->stmt,i_col);
          dbsql_str_appendf(out, "(%d-byte blob)", n_blob);
          break;
        }
        default: {
          const char *txt = (const char*)dbsql_column_text(p->stmt,i_col);
          __qrf_encode_text(p, out, txt);
        }
      }
      break;
    }
    case DBSQL_NULL: {
      dbsql_str_appendall(out, p->spec.null);
      break;
    }
    case DBSQL_TEXT: {
      const char *txt = (const char*)dbsql_column_text(p->stmt,i_col);
      __qrf_encode_text(p, out, txt);
      break;
    }
  }
#if DBSQL_VERSION_NUMBER>=3052000
  if( p->spec.n_char_limit>0
   && (dbsql_str_length(out) - i_start_len) > p->spec.n_char_limit
  ){
    const unsigned char *z;
    int ii = 0, w = 0, limit = p->spec.n_char_limit;
    z = (const unsigned char*)dbsql_str_value(out) + i_start_len;
    if( limit<4 ) limit = 4;
    while( 1 ){
      if( z[ii]<' ' ){
        int k;
        if( z[ii]=='\033' && (k = __qrf_is_vt100(z+ii))>0 ){
          ii += k;
        }else if( z[ii]==0 ){
          break;
        }else{
          ii++;
        }
      }else if( (0x80&z[ii])==0 ){
        w++;
        if( w>limit ) break;
        ii++;
      }else{
        int u = 0;
        int len = dbsql_qrf_decode_utf8(&z[ii], &u);
        w += dbsql_qrf_wcwidth(u);
        if( w>limit ) break;
        ii += len;
      }
    }
    if( w>limit ){
      dbsql_str_truncate(out, i_start_len+ii);
      dbsql_str_append(out, "...", 3);
    }
  }
#endif
}

/* Trim spaces of the end if out
*/
static void __qrf_r_trim(dbsql_str_t *out){
#if DBSQL_VERSION_NUMBER>=3052000
  int n_byte = dbsql_str_length(out);
  const char *z_out = dbsql_str_value(out);
  while( n_byte>0 && z_out[n_byte-1]==' ' ){ n_byte--; }
  dbsql_str_truncate(out, n_byte);
#endif
}

/*
** Store string utf to out as w characters.  If w is negative,
** then right-justify the text.  W is the width in display characters, not
** in bytes.  Double-width unicode characters count as two characters.
** VT100 escape sequences count as zero.  And so forth.
*/
static void __qrf_width_print(qrf_t *p, dbsql_str_t *out, int w, const char *utf){
  const unsigned char *a = (const unsigned char*)utf;
  static const int mx_w = 10000000;
  unsigned char c;
  int i = 0;
  int n = 0;
  int k;
  int aw;
  (void)p;
  if( w<-mx_w ){
    w = -mx_w;
  }else if( w>mx_w ){
    w= mx_w;
  }
  aw = w<0 ? -w : w;
  if( a==0 ) a = (const unsigned char*)"";
  while( (c = a[i])!=0 ){
    if( (c&0xc0)==0xc0 ){
      int u;
      int len = dbsql_qrf_decode_utf8(a+i, &u);
      int x = dbsql_qrf_wcwidth(u);
      if( x+n>aw ){
        break;
      }
      i += len;
      n += x;
    }else if( c==0x1b && (k = __qrf_is_vt100(&a[i]))>0 ){
      i += k;       
    }else if( n>=aw ){
      break;
    }else{
      n++;
      i++;
    }
  }
  if( n>=aw ){
    dbsql_str_append(out, utf, i);
  }else if( w<0 ){
    if( aw>n ) dbsql_str_appendchar(out, aw-n, ' ');
    dbsql_str_append(out, utf, i);
  }else{
    dbsql_str_append(out, utf, i);
    if( aw>n ) dbsql_str_appendchar(out, aw-n, ' ');
  }
}

/*
** (*pz)[] is a line of text that is to be displayed the box or table or
** similar tabular formats.  z[] contain newlines or might be too wide
** to fit in the columns so will need to be split into multiple line.
**
** This routine determines:
**
**    *  How many bytes of z[] should be shown on the current line.
**    *  How many character positions those bytes will cover.
**    *  The byte offset to the start of the next line.
*/
static void __qrf_wrap_line(
  const char *in,   /* Input text to be displayed */
  int w,             /* column_t width in characters (not bytes) */
  int wrap,         /* True if we should do word-wrapping */
  int *pn_this,       /* OUT: How many bytes of z[] for the current line */
  int *pn_wide,       /* OUT: How wide is the text of this line */
  int *pi_next        /* OUT: Offset into z[] to start of the next line */
){
  int i;                 /* Input bytes consumed */
  int k;                 /* Bytes in a VT100 code */
  int n;                 /* Output column number */
  const unsigned char *z = (const unsigned char*)in;
  unsigned char c = 0;

  if( z[0]==0 ){
    *pn_this = 0;
    *pn_wide = 0;
    *pi_next = 0;
    return;
  }
  n = 0;
  for(i=0; n<=w; i++){
    c = z[i];
    if( c>=0xc0 ){
      int u;
      int len = dbsql_qrf_decode_utf8(&z[i], &u);
      int wcw = dbsql_qrf_wcwidth(u);
      if( wcw+n>w ) break;
      i += len-1;
      n += wcw;
      continue;
    }
    if( c>=' ' ){
      if( n==w ) break;
      n++;
      continue;
    }
    if( c==0 || c=='\n' ) break;
    if( c=='\r' && z[i+1]=='\n' ){ c = z[++i]; break; }
    if( c=='\t' ){
      int wcw = 8 - (n&7);
      if( n+wcw>w ) break;
      n += wcw;
      continue;
    }
    if( c==0x1b && (k = __qrf_is_vt100(&z[i]))>0 ){
      i += k-1;
    }else if( n==w ){
      break;
    }else{
      n++;
    }
  }
  if( c==0 ){
    *pn_this = i;
    *pn_wide = n;
    *pi_next = i;
    return;
  }
  if( c=='\n' ){
    *pn_this = i;
    *pn_wide = n;
    *pi_next = i+1;
    return;
  }

  /* If we get this far, that means the current line will end at some
  ** point that is neither a "\n" or a 0x00.  Figure out where that
  ** split should occur
  */
  if( wrap && z[i]!=0 && !QRF_SPACE(z[i]) && QRF_ALNUM(c)==QRF_ALNUM(z[i]) ){
    /* Perhaps try to back up to a better place to break the line */
    for(k=i-1; k>=i/2; k--){
      if( QRF_SPACE(z[k]) ) break;
    }
    if( k<i/2 && i/2>0 ){
      for(k=i; k>=i/2; k--){
        if( QRF_ALNUM(z[k-1])!=QRF_ALNUM(z[k]) && (z[k]&0xc0)!=0x80 ) break;
      }
    }
    if( k>=i/2 ){
      i = k;
      n = __qrf_display_width((const char*)z, k, 0);
    }
  }
  *pn_this = i;
  *pn_wide = n;
  while( in[i]==' ' || in[i]=='\t' || in[i]=='\r' ){ i++; }
  *pi_next = i;
}

/*
** Append n_val bytes of text from val onto the end of out.
** Convert tab characters in val to the appropriate number of
** spaces.
*/
static void __qrf_append_with_tabs(
  dbsql_str_t *out,       /* Append text here */
  const char *val,        /* Text to append */
  int n_val                 /* Use only the first n_val bytes of val[] */
){
  int i = 0;
  unsigned int col = 0;
  unsigned char *z = (unsigned char *)val;
  while( i<n_val ){
    unsigned char c = z[i];
    if( c<' ' ){
      int k;
      dbsql_str_append(out, (const char*)z, i);
      n_val -= i;
      z += i;
      i = 0;
      if( c=='\033' && (k = __qrf_is_vt100(z))>0 ){
        dbsql_str_append(out, (const char*)z, k);
        z += k;
        n_val -= k;
      }else if( c=='\t' ){
        k = 8 - (col&7);
        dbsql_str_appendchar(out, k, ' ');
        col += k;
        z++;
        n_val--;
      }else if( c=='\r' && n_val==1 ){
        z++;
        n_val--;
      }else{
        char ctrl_pik[4];
        col++;
        ctrl_pik[0] = 0xe2;
        ctrl_pik[1] = 0x90;
        ctrl_pik[2] = 0x80+c;
        dbsql_str_append(out, ctrl_pik, 3);
        z++;
        n_val--;
      }
    }else if( (0x80&c)==0 ){
      i++;
      col++;
    }else{
      int u = 0;
      int len = dbsql_qrf_decode_utf8(&z[i], &u);
      i += len;
      col += dbsql_qrf_wcwidth(u);
    }
  }
  dbsql_str_append(out, (const char*)z, i);
}    

/*
** GCC does not define the offsetof() macro so we'll have to do it
** ourselves.
*/
#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif

/*
** Data for columnar layout, collected into a single object so
** that it can be more easily passed into subroutines.
*/
typedef struct qrf_col_data qrf_col_data;
struct qrf_col_data {
  qrf_t *p;                  /* The QRF instance */
  int n_col;                /* Number of columns in the table */
  unsigned char multi_row; /* One or more cells will span multiple lines */
  unsigned char n_margin;   /* Width of column margins */
  int64_t n_row;      /* Number of rows */
  int64_t n_alloc;    /* Number of cells allocated */
  int64_t n;         /* Number of cells.  n_col*n_row */
  char **az;               /* Content of all cells */
  int *wths;              /* Width of each cell */
  unsigned char *ab_num;    /* True for each numeric cell */
  struct qrf_per_col {       /* Per-column data */
    char *z;                 /* Cache of text for current row */
    int w;                   /* Computed width of this column */
    int mx_w;                 /* Maximum natural (unwrapped) width */
    unsigned char e;         /* Alignment */
    unsigned char fx;        /* Width is fixed */
    unsigned char num;      /* True if is numeric */
  } *a;                    /* One per column */
};

/*
** Output horizontally justified text into out.  The text is the
** first n_val bytes of val.  Include n_ws bytes of whitespace, either
** split between both sides, or on the left, or on the right, depending
** on align.
*/
static void __qrf_print_aligned(
  dbsql_str_t *out,       /* Append text here */
  struct qrf_per_col *col,  /* Information about the text to print */
  int n_val,                /* Use only the first n_val bytes of val[] */
  int n_ws                 /* Whitespace for horizonal alignment */
){
  unsigned char align = col->e & QRF_ALIGN_HMASK;
  if( align==QRF_Auto && col->num ) align = QRF_ALIGN_Right;
  if( align==QRF_ALIGN_Center ){
    /* Center the text */
    dbsql_str_appendchar(out, n_ws/2, ' ');
    __qrf_append_with_tabs(out, col->z, n_val);
    dbsql_str_appendchar(out, n_ws - n_ws/2, ' ');
  }else if( align==QRF_ALIGN_Right ){
    /* Right justify the text */
    dbsql_str_appendchar(out, n_ws, ' ');
    __qrf_append_with_tabs(out, col->z, n_val);
  }else{
    /* Left justify the text */
    __qrf_append_with_tabs(out, col->z, n_val);
    dbsql_str_appendchar(out, n_ws, ' ');
  }
}

/*
** Free all the memory allocates in the qrf_col_data object
*/
static void __qrf_col_data_free(qrf_col_data *p){
  int64_t i;
  for(i=0; i<p->n; i++) dbsql_free(p->az[i]);
  dbsql_free(p->az);
  dbsql_free(p->wths);
  dbsql_free(p->ab_num);
  dbsql_free(p->a);
  memset(p, 0, sizeof(*p));
}

/*
** Allocate space for more cells in the qrf_col_data object.
** Return non-zero if a memory allocation fails.
*/
static int __qrf_col_data_enlarge(qrf_col_data *p){
  char **data;
  int *wths;
  unsigned char *ab_num;
  p->n_alloc = 2*p->n_alloc + 10*p->n_col;
  data = dbsql_realloc64(p->az, p->n_alloc*sizeof(char*));
  if( data==0 ){
    __qrf_oom(p->p);
    __qrf_col_data_free(p);
    return 1;
  }
  p->az = data;
  wths = dbsql_realloc64(p->wths, p->n_alloc*sizeof(int));
  if( wths==0 ){
    __qrf_oom(p->p);
    __qrf_col_data_free(p);
    return 1;
  }
  p->wths = wths;
  ab_num = dbsql_realloc64(p->ab_num, p->n_alloc);
  if( ab_num==0 ){
    __qrf_oom(p->p);
    __qrf_col_data_free(p);
    return 1;
  }
  p->ab_num = ab_num;
  return 0;
}

/*
** Print a markdown or table-style row separator using ascii-art
*/
static void __qrf_row_separator(dbsql_str_t *out, qrf_col_data *p, char c_sep){
  int i;
  if( p->n_col>0 ){
    int use_border = p->p->spec.border!=QRF_No;
    if( use_border ){
      dbsql_str_append(out, &c_sep, 1);
    }
    dbsql_str_appendchar(out, p->a[0].w+p->n_margin, '-');
    for(i=1; i<p->n_col; i++){
      dbsql_str_append(out, &c_sep, 1);
      dbsql_str_appendchar(out, p->a[i].w+p->n_margin, '-');
    }
    if( use_border ){
      dbsql_str_append(out, &c_sep, 1);
    }
  }
  dbsql_str_append(out, "\n", 1);
}

/*
** UTF8 box-drawing characters.  Imagine box lines like this:
**
**           1
**           |
**       4 --+-- 2
**           |
**           3
**
** Each box characters has between 2 and 4 of the lines leading from
** the center.  The characters are here identified by the numbers of
** their corresponding lines.
*/
#define BOX_24   "\342\224\200"  /* U+2500 --- */
#define BOX_13   "\342\224\202"  /* U+2502  |  */
#define BOX_23   "\342\224\214"  /* U+250c  ,- */
#define BOX_34   "\342\224\220"  /* U+2510 -,  */
#define BOX_12   "\342\224\224"  /* U+2514  '- */
#define BOX_14   "\342\224\230"  /* U+2518 -'  */
#define BOX_123  "\342\224\234"  /* U+251c  |- */
#define BOX_134  "\342\224\244"  /* U+2524 -|  */
#define BOX_234  "\342\224\254"  /* U+252c -,- */
#define BOX_124  "\342\224\264"  /* U+2534 -'- */
#define BOX_1234 "\342\224\274"  /* U+253c -|- */

/* Rounded corners: */
#define BOX_R12  "\342\225\260"  /* U+2570  '- */
#define BOX_R23  "\342\225\255"  /* U+256d  ,- */
#define BOX_R34  "\342\225\256"  /* U+256e -,  */
#define BOX_R14  "\342\225\257"  /* U+256f -'  */

/* Doubled horizontal lines: */
#define DBL_24   "\342\225\220"  /* U+2550 === */
#define DBL_123  "\342\225\236"  /* U+255e  |= */
#define DBL_134  "\342\225\241"  /* U+2561 =|  */
#define DBL_1234 "\342\225\252"  /* U+256a =|= */

/* Draw horizontal line N characters long using unicode box
** characters
*/
static void __qrf_box_line(dbsql_str_t *out, int N, int dbl){
  const char *dashes[2] = {
      BOX_24 BOX_24 BOX_24 BOX_24 BOX_24   BOX_24 BOX_24 BOX_24 BOX_24 BOX_24,
      DBL_24 DBL_24 DBL_24 DBL_24 DBL_24   DBL_24 DBL_24 DBL_24 DBL_24 DBL_24
  };/*  0       1      2     3      4        5      6      7      8      9   */
  const int n_dash = 30;
  N *= 3;
  while( N>n_dash ){
    dbsql_str_append(out, dashes[dbl], n_dash);
    N -= n_dash;
  }
  dbsql_str_append(out, dashes[dbl], N);
}

/*
** Draw a horizontal separator for a QRF_STYLE_Box table.
*/
static void __qrf_box_separator(
  dbsql_str_t *out,
  qrf_col_data *p,
  const char *sep1,
  const char *sep2,
  const char *sep3,
  int dbl
){
  int i;
  if( p->n_col>0 ){
    int use_border = p->p->spec.border!=QRF_No;
    if( use_border ){
      dbsql_str_appendall(out, sep1);
    }
    __qrf_box_line(out, p->a[0].w+p->n_margin, dbl);
    for(i=1; i<p->n_col; i++){
      dbsql_str_appendall(out, sep2);
      __qrf_box_line(out, p->a[i].w+p->n_margin, dbl);
    }
    if( use_border ){
      dbsql_str_appendall(out, sep3);
    }
  }
  dbsql_str_append(out, "\n", 1);
}

/*
** Load into data the default alignment for the body of a table.
*/
static void __qrf_load_alignment(qrf_col_data *data, qrf_t *p){
  int64_t i;
  for(i=0; i<data->n_col; i++){
    data->a[i].e = p->spec.dflt_align;
    if( i<p->spec.n_align ){
      unsigned char ax = p->spec.aligns[i];
      if( (ax & QRF_ALIGN_HMASK)!=0 ){
        data->a[i].e = (ax & QRF_ALIGN_HMASK) |
                            (data->a[i].e & QRF_ALIGN_VMASK);
      }
    }else if( i<p->spec.n_width ){
      if( p->spec.widths[i]<0 ){
         data->a[i].e = QRF_ALIGN_Right |
                               (data->a[i].e & QRF_ALIGN_VMASK);
      }
    }
  }
}

/*
** If the single column in data->a[] with data->n entries can be
** laid out as n_col columns with a 2-space gap between each such
** that all columns fit within n_sw, then return a pointer to an array
** of integers which is the width of each column from left to right.
**
** If the layout is not possible, return a NULL pointer.
**
** Space to hold the returned array is from dbsql_malloc64().
*/
static int *__qrf_valid_layout(
  qrf_col_data *data,   /* Collected query results */
  qrf_t *p,              /* On which to report an OOM */
  int n_col,            /* Attempt this many columns */
  int n_sw              /* Screen width */
){
  int i;        /* Loop counter */
  int nr;       /* Number of rows */
  int w = 0;    /* Width of the current column */
  int t;        /* Total width of all columns */
  int *aw;      /* Array of individual column widths */

  aw = dbsql_malloc64( sizeof(int)*n_col );
  if( aw==0 ){
    __qrf_oom(p);
    return 0;
  }
  nr = (data->n + n_col - 1)/n_col;
  for(i=0; i<data->n; i++){
    if( (i%nr)==0 ){
      if( i>0 ) aw[i/nr-1] = w;
      w = data->wths[i];
    }else if( data->wths[i]>w ){
      w = data->wths[i];
    }
  }
  aw[n_col-1] = w;
  for(t=i=0; i<n_col; i++) t += aw[i];
  t += 2*(n_col-1);
  if( t>n_sw ){
    dbsql_free(aw);
    return 0;
  }
  return aw;
}

/*
** The output is single-column and the split_column flag is set.
** Check to see if the single-column output can be split into multiple
** columns that appear side-by-side.  Adjust data appropriately.
*/
static void __qrf_split_column(qrf_col_data *data, qrf_t *p){
  int n_col = 1;
  int *aw = 0;
  char **az = 0;
  int *wths = 0;
  unsigned char *ab_num = 0;
  int n_col_next = 2;
  int w;
  struct qrf_per_col *a = 0;
  int64_t n_row = 1;
  int64_t i;
  while( 1/*exit-by-break*/ ){
    int *aw_new = __qrf_valid_layout(data, p, n_col_next, p->spec.n_screen_width);
    if( aw_new==0 ) break;
    dbsql_free(aw);
    aw = aw_new;
    n_col = n_col_next;
    n_row = (data->n + n_col - 1)/n_col;
    if( n_row==1 ) break;
    n_col_next++;
    while( (data->n + n_col_next - 1)/n_col_next == n_row ) n_col_next++;
  }
  if( n_col==1 ){
    dbsql_free(aw);
    return;  /* Cannot do better than 1 column */
  }
  az = dbsql_malloc64( n_row*n_col*sizeof(char*) );
  if( az==0 ){
    __qrf_oom(p);
    return;
  }
  wths = dbsql_malloc64( n_row*n_col*sizeof(int) );
  if( wths==0 ){
    dbsql_free(az);
    __qrf_oom(p);
    return;
  }
  a = dbsql_malloc64( n_col*sizeof(struct qrf_per_col) );
  if( a==0 ){
    dbsql_free(az);
    dbsql_free(wths);
    __qrf_oom(p);
    return;
  }
  ab_num = dbsql_malloc64( n_row*n_col );
  if( ab_num==0 ){
    dbsql_free(az);
    dbsql_free(wths);
    dbsql_free(a);
    __qrf_oom(p);
    return;
  }
  for(i=0; i<data->n; i++){
    int64_t j = (i%n_row)*n_col + (i/n_row);
    az[j] = data->az[i];
    ab_num[j]= data->ab_num[i];
    data->az[i] = 0;
    wths[j] = data->wths[i];
  }
  while( i<n_row*n_col ){
    int64_t j = (i%n_row)*n_col + (i/n_row);
    az[j] = dbsql_mprintf("");
    if( az[j]==0 ) __qrf_oom(p);
    wths[j] = 0;
    ab_num[j] = 0;
    i++;
  }
  for(i=0; i<n_col; i++){
    a[i].fx = a[i].mx_w = a[i].w = aw[i];
    a[i].e = data->a[0].e;
  }
  dbsql_free(data->az);
  dbsql_free(data->wths);
  dbsql_free(data->a);
  dbsql_free(data->ab_num);
  dbsql_free(aw);
  data->az = az;
  data->wths = wths;
  data->a = a;
  data->ab_num = ab_num;
  data->n_col = n_col;
  data->n = data->n_alloc = n_row*n_col;
  for(i=w=0; i<n_col; i++) w += a[i].w;
  data->n_margin = (p->spec.n_screen_width - w)/(n_col - 1);
  if( data->n_margin>5 ) data->n_margin = 5;
}

/*
** Adjust the layout for the screen width restriction
*/
static void __qrf_restrict_screen_width(qrf_col_data *data, qrf_t *p){
  int sep_w;             /* Width of all box separators and margins */
  int sum_w;             /* Total width of data area over all columns */
  int target_w;          /* Desired total data area */
  int i;                /* Loop counters */
  int n_col;             /* Number of columns */

  data->n_margin = 2;   /* Default to normal margins */
  if( p->spec.n_screen_width==0 ) return;
  if( p->spec.style==QRF_STYLE_Column ){
    sep_w = data->n_col*2 - 2;
  }else{
    sep_w = data->n_col*3 + 1;
    if( p->spec.border==QRF_No ) sep_w -= 2;
  }
  n_col = data->n_col;
  for(i=sum_w=0; i<n_col; i++) sum_w += data->a[i].w;
  if( p->spec.n_screen_width >= sum_w+sep_w ) return;

  /* First thing to do is reduce the separation between columns */
  data->n_margin = 0;
  if( p->spec.style==QRF_STYLE_Column ){
    sep_w = data->n_col - 1;
  }else{
    sep_w = data->n_col + 1;
    if( p->spec.border==QRF_No ) sep_w -= 2;
  }
  target_w = p->spec.n_screen_width - sep_w;

#define MIN_SQUOZE    8
#define MIN_EX_SQUOZE 16
  /* Reduce the width of the widest eligible column.  A column is
  ** eligible for narrowing if:
  **
  **    *  It is not a fixed-width column  (a[0].fx is false)
  **    *  The current width is more than MIN_SQUOZE
  **    *  Either:
  **         +  The current width is more then MIN_EX_SQUOZE, or
  **         +  The current width is more than half the max width (a[].mx_w)
  **
  ** Keep making reductions until either no more reductions are
  ** possible or until the size target is reached.
  */
  while( sum_w > target_w ){
    int gain, w;
    int ix = -1;
    int mx = 0;
    for(i=0; i<n_col; i++){
      if( data->a[i].fx==0
       && (w = data->a[i].w)>mx
       && w>MIN_SQUOZE
       && (w>MIN_EX_SQUOZE || w*2>data->a[i].mx_w)
      ){
        ix = i;
        mx = w;
      }
    }
    if( ix<0 ) break;
    if( mx>=MIN_SQUOZE*2 ){
      gain = mx/2;
    }else{
      gain = mx - MIN_SQUOZE;
    }
    if( sum_w - gain < target_w ){
      gain = sum_w - target_w;
    }
    sum_w -= gain;
    data->a[ix].w -= gain;
    data->multi_row = 1;
  }
}

/*
** Columnar modes require that the entire query be evaluated first, with
** results written into memory, so that we can compute appropriate column
** widths.
*/
static void __qrf_columnar(qrf_t *p){
  int64_t i, j;                     /* Loop counters */
  const char *col_sep = 0;                 /* column_t separator text */
  const char *row_sep = 0;                 /* Row terminator text */
  const char *row_start = 0;               /* Row start text */
  int sz_col_sep, sz_row_sep, sz_row_start;     /* Size in bytes of previous 3 */
  int rc;                                 /* Result code */
  int n_column = p->n_col;                  /* Number of columns */
  int ww;                                /* True to do word-wrap */
  dbsql_str_t *str;                      /* Temporary rendering */
  qrf_col_data data;                        /* Columnar layout data */
  int r_trim;                             /* Trim trailing space */

  rc = dbsql_step(p->stmt);
  if( rc!=DBSQL_ROW || n_column==0 ){
    return;   /* No output */
  }

  /* Initialize the data container */
  memset(&data, 0, sizeof(data));
  data.n_col = p->n_col;
  data.p = p;
  data.a = dbsql_malloc64( n_column*sizeof(struct qrf_per_col) );
  if( data.a==0 ){
    __qrf_oom(p);
    return;
  }
  memset(data.a, 0, n_column*sizeof(struct qrf_per_col) );
  if( __qrf_col_data_enlarge(&data) ) return;
  assert( data.az!=0 );

  /* Load the column header names and all cell content into data */
  if( p->spec.titles==QRF_Yes ){
    unsigned char saved_e_text = p->spec.text;
    p->spec.text = p->spec.title;
    memset(data.ab_num, 0, n_column);
    for(i=0; i<n_column; i++){
      const char *z = (const char*)dbsql_column_name(p->stmt,i);
      int n_nl = 0;
      int n, w;
      str = dbsql_str_new(p->db);
      __qrf_encode_text(p, str, z ? z : "");
      n = dbsql_str_length(str);
      __qrf_str_err(p, str);
      z = data.az[data.n] = dbsql_str_finish(str);
      if( p->spec.n_title_limit ){
        n_nl = 0;
        data.wths[data.n] = w = __qrf_title_limit(data.az[data.n],
                                               p->spec.n_title_limit );
      }else{
        data.wths[data.n] = w = __qrf_display_width(z, n, &n_nl);
      }
      data.n++;
      if( w>data.a[i].mx_w ) data.a[i].mx_w = w;
      if( n_nl ) data.multi_row = 1;
    }
    p->spec.text = saved_e_text;
    p->n_row++;
  }
  do{
    if( data.n+n_column > data.n_alloc ){
      if( __qrf_col_data_enlarge(&data) ) return;
    }
    for(i=0; i<n_column; i++){
      char *z;
      int n_nl = 0;
      int n, w;
      int type = dbsql_column_type(p->stmt,i);
      str = dbsql_str_new(p->db);
      __qrf_render_value(p, str, i);
      n = dbsql_str_length(str);
      __qrf_str_err(p, str);
      z = data.az[data.n] = dbsql_str_finish(str);
      data.ab_num[data.n] = type==DBSQL_INTEGER || type==DBSQL_FLOAT;
      data.wths[data.n] = w = __qrf_display_width(z, n, &n_nl);
      data.n++;
      if( w>data.a[i].mx_w ) data.a[i].mx_w = w;
      if( n_nl ) data.multi_row = 1;
    }
    p->n_row++;
  }while( dbsql_step(p->stmt)==DBSQL_ROW && p->i_err==DBSQL_OK );
  if( p->i_err ){
    __qrf_col_data_free(&data);
    return;
  }

  /* Compute the width and alignment of every column */
  if( p->spec.titles==QRF_No ){
    __qrf_load_alignment(&data, p);
  }else{
    unsigned char e;
    if( p->spec.title_align==QRF_Auto ){
      e = QRF_ALIGN_Center;
    }else{
      e = p->spec.title_align;
    }
    for(i=0; i<n_column; i++) data.a[i].e = e;
  }

  for(i=0; i<n_column; i++){
    int w = 0;
    if( i<p->spec.n_width ){
      w = p->spec.widths[i];
      if( w==(-32768) ){
        w = 0;
        if( p->spec.n_align>i && (p->spec.aligns[i] & QRF_ALIGN_HMASK)==0 ){
          data.a[i].e |= QRF_ALIGN_Right;
        }
      }else if( w<0 ){
        w = -w;
        if( p->spec.n_align>i && (p->spec.aligns[i] & QRF_ALIGN_HMASK)==0 ){
          data.a[i].e |= QRF_ALIGN_Right;
        }
      }
      if( w ) data.a[i].fx = 1;
    }
    if( w==0 ){
      w = data.a[i].mx_w;
      if( p->spec.n_wrap>0 && w>p->spec.n_wrap ){
        w = p->spec.n_wrap;
        data.multi_row = 1;
      }
    }else if( (data.multi_row==0 || w==1) && data.a[i].mx_w>w ){
      data.multi_row = 1;
      if( w==1 ){
        /* If wths[j] is 2 or more, then there might be a double-wide
        ** character somewhere.  So make the column width at least 2. */
        w = 2;
      }
    }
    data.a[i].w = w;
  }

  if( n_column==1
   && data.n>1
   && p->spec.split_column==QRF_Yes
   && p->spec.style==QRF_STYLE_Column
   && p->spec.titles==QRF_No
   && p->spec.n_screen_width>data.a[0].w+3
  ){
    /* Attempt to convert single-column tables into multi-column by
    ** verticle wrapping, if the screen is wide enough and if the
    ** split_column flag is set. */
    __qrf_split_column(&data, p);
    n_column = data.n_col;
  }else{
    /* Adjust the column widths due to screen width restrictions */
    __qrf_restrict_screen_width(&data, p);
  }

  /* Draw the line across the top of the table.  Also initialize
  ** the row boundary and column separator texts. */
  switch( p->spec.style ){
    case QRF_STYLE_Box:
      if( data.n_margin ){
        row_start = BOX_13 " ";
        col_sep = " " BOX_13 " ";
        row_sep = " " BOX_13 "\n";
      }else{
        row_start = BOX_13;
        col_sep = BOX_13;
        row_sep = BOX_13 "\n";
      }
      if( p->spec.border==QRF_No){
        row_start += 3;
        row_sep = "\n";
      }else{
        __qrf_box_separator(p->out, &data, BOX_R23, BOX_234, BOX_R34, 0);
      }
      break;
    case QRF_STYLE_Table:
      if( data.n_margin ){
        row_start = "| ";
        col_sep = " | ";
        row_sep = " |\n";
      }else{
        row_start = "|";
        col_sep = "|";
        row_sep = "|\n";
      }
      if( p->spec.border==QRF_No ){
        row_start += 1;
        row_sep = "\n";
      }else{
        __qrf_row_separator(p->out, &data, '+');
      }
      break;
    case QRF_STYLE_Column: {
      static const char space[] = "     ";
      row_start = "";
      if( data.n_margin<2 ){
        col_sep = " ";
      }else if( data.n_margin<=5 ){
        col_sep = &space[5-data.n_margin];
      }else{
        col_sep = space;
      }
      row_sep = "\n";
      break;
    }
    default:  /*case QRF_STYLE_Markdown:*/
      if( data.n_margin ){
        row_start = "| ";
        col_sep = " | ";
        row_sep = " |\n";
      }else{
        row_start = "|";
        col_sep = "|";
        row_sep = "|\n";
      }
      break;
  }
  sz_row_start = (int)strlen(row_start);
  sz_row_sep = (int)strlen(row_sep);
  sz_col_sep = (int)strlen(col_sep);

  ww = (p->spec.word_wrap==QRF_Yes && data.multi_row);
  if( p->spec.style==QRF_STYLE_Column
   || (p->spec.border==QRF_No
       && (p->spec.style==QRF_STYLE_Box || p->spec.style==QRF_STYLE_Table)
      )
  ){
    r_trim = 1;
  }else{
    r_trim = 0;
  }
  for(i=0; i<data.n && dbsql_str_errcode(p->out)==DBSQL_OK; i+=n_column){
    int more;
    int n_row = 0;

    /* Draw a single row of the table.  This might be the title line
    ** (if there is a title line) or a row in the body of the table.
    ** The column number will be j.  The row number is i/n_column.
    */
    for(j=0; j<n_column; j++){
      data.a[j].z = data.az[i+j];
      if( data.a[j].z==0 ) data.a[j].z = "";
      data.a[j].num = data.ab_num[i+j];
    }
    do{
      dbsql_str_append(p->out, row_start, sz_row_start);
      more = 0;
      for(j=0; j<n_column; j++){
        int n_this = 0;
        int n_wide = 0;
        int i_next = 0;
        int n_ws;
        __qrf_wrap_line(data.a[j].z, data.a[j].w, ww, &n_this, &n_wide, &i_next);
        n_ws = data.a[j].w - n_wide;
        __qrf_print_aligned(p->out, &data.a[j], n_this, n_ws);
        data.a[j].z += i_next;
        if( data.a[j].z[0]!=0 ){
          more = 1;
        }
        if( j<n_column-1 ){
          dbsql_str_append(p->out, col_sep, sz_col_sep);
        }else{
          if( r_trim ) __qrf_r_trim(p->out);
          dbsql_str_append(p->out, row_sep, sz_row_sep);
        }
      }
    }while( more && ++n_row < p->mx_height );
    if( more ){
      /* This row was terminated by n_line_limit.  Show ellipsis. */
      dbsql_str_append(p->out, row_start, sz_row_start);
      for(j=0; j<n_column; j++){
        if( data.a[j].z[0]==0 ){
          dbsql_str_appendchar(p->out, data.a[j].w, ' ');
        }else{
          int n_e = 3;
          if( n_e>data.a[j].w ) n_e = data.a[j].w;
          data.a[j].z = "...";
          __qrf_print_aligned(p->out, &data.a[j], n_e, data.a[j].w-n_e);
        }
        if( j<n_column-1 ){
          dbsql_str_append(p->out, col_sep, sz_col_sep);
        }else{
          if( r_trim ) __qrf_r_trim(p->out);
          dbsql_str_append(p->out, row_sep, sz_row_sep);
        }
      }
    }

    /* Draw either (1) the separator between the title line and the body
    ** of the table, or (2) separators between individual rows of the table
    ** body.  is_title_data_separator will be true if we are doing (1).
    */
    if( (i==0 || data.multi_row) && i+n_column<data.n ){
      int is_title_data_separator = (i==0 && p->spec.titles==QRF_Yes);
      if( is_title_data_separator ){
        __qrf_load_alignment(&data, p);
      }
      switch( p->spec.style ){
        case QRF_STYLE_Table: {
          if( is_title_data_separator || data.multi_row ){
            __qrf_row_separator(p->out, &data, '+');
          }
          break;
        }
        case QRF_STYLE_Box: {
          if( is_title_data_separator ){
            __qrf_box_separator(p->out, &data, DBL_123, DBL_1234, DBL_134, 1);
          }else if( data.multi_row ){
            __qrf_box_separator(p->out, &data, BOX_123, BOX_1234, BOX_134, 0);
          }
          break;
        }
        case QRF_STYLE_Markdown: {
          if( is_title_data_separator ){
            __qrf_row_separator(p->out, &data, '|');
          }
          break;
        }
        case QRF_STYLE_Column: {
          if( is_title_data_separator ){
            for(j=0; j<n_column; j++){
              dbsql_str_appendchar(p->out, data.a[j].w, '-');
              if( j<n_column-1 ){
                dbsql_str_append(p->out, col_sep, sz_col_sep);
              }else{
                __qrf_r_trim(p->out);
                dbsql_str_append(p->out, row_sep, sz_row_sep);
              }
            }
          }else if( data.multi_row ){
            __qrf_r_trim(p->out);
            dbsql_str_append(p->out, "\n", 1);
          }
          break;
        }
      }
    }
  }

  /* Draw the line across the bottom of the table */
  if( p->spec.border!=QRF_No ){
    switch( p->spec.style ){
      case QRF_STYLE_Box:
        __qrf_box_separator(p->out, &data, BOX_R12, BOX_124, BOX_R14, 0);
        break;
      case QRF_STYLE_Table:
        __qrf_row_separator(p->out, &data, '+');
        break;
    }
  }
  __qrf_write(p);

  __qrf_col_data_free(&data);
  return;
}

/*
** Parameter arrays points to a zero-terminated array of strings. str
** points to a single nul-terminated string. Return non-zero if str
** is equal, according to strcmp(), to any of the strings in the array.
** Otherwise, return zero.
*/
static int __qrf_string_in_array(const char *str, const char **arrays){
  int i;
  if( str==0 ) return 0;
  for(i=0; arrays[i]; i++){
    if( 0==strcmp(str, arrays[i]) ) return 1;
  }
  return 0;
}

/*
** Print out an EXPLAIN with indentation.  This is a two-pass algorithm.
**
** On the first pass, we compute indents[i_op] which is the amount of
** indentation to apply to the i_op-th opcode.  The output actually occurs
** on the second pass.
**
** The indenting rules are:
**
**     * For each "Next", "Prev", "VNext" or "VPrev" instruction, indent
**       all opcodes that occur between the p2 jump destination and the opcode
**       itself by 2 spaces.
**
**     * Do the previous for "Return" instructions for when P2 is positive.
**       See tag-20220407a in wherecode.c and vdbe.c.
**
**     * For each "Goto", if the jump destination is earlier in the program
**       and ends on one of:
**          Yield  __seek_gt  __seek_lt  __row_set_read  Rewind
**       or if the P1 parameter is one instead of zero,
**       then indent all opcodes between the earlier instruction
**       and "Goto" by 2 spaces.
*/
static void __qrf_explain(qrf_t *p){
  int *ab_yield = 0;     /* ab_yield[i_op] is rue if opcode i_op is an OP_Yield */
  int *indents = 0;    /* Indent the i_op-th opcode by indents[i_op] */
  int64_t n_alloc = 0;       /* Allocated size of indents[], ab_yield */
  int n_indent = 0;      /* Number of entries in indents[] */
  int i_op;              /* Opcode number */
  int i;                /* column_t loop counter */

  const char *nexts[] = { "Next", "Prev", "VPrev", "VNext", "SorterNext",
                           "Return", 0 };
  const char *yields[] = { "Yield", "SeekLT", "SeekGT", "RowSetRead",
                            "Rewind", 0 };
  const char *gotos[] = { "Goto", 0 };

  /* The caller guarantees that the leftmost 4 columns of the statement
  ** passed to this function are equivalent to the leftmost 4 columns
  ** of EXPLAIN statement output. In practice the statement may be
  ** an EXPLAIN, or it may be a query on the bytecode() virtual table.  */
  assert( dbsql_column_count(p->stmt)>=4 );
  assert( 0==dbsql_stricmp( dbsql_column_name(p->stmt, 0), "addr" ) );
  assert( 0==dbsql_stricmp( dbsql_column_name(p->stmt, 1), "opcode" ) );
  assert( 0==dbsql_stricmp( dbsql_column_name(p->stmt, 2), "p1" ) );
  assert( 0==dbsql_stricmp( dbsql_column_name(p->stmt, 3), "p2" ) );

  for(i_op=0; DBSQL_ROW==dbsql_step(p->stmt) && !p->i_err; i_op++){
    int i_addr = dbsql_column_int(p->stmt, 0);
    const char *op = (const char*)dbsql_column_text(p->stmt, 1);
    int p1 = dbsql_column_int(p->stmt, 2);
    int p2 = dbsql_column_int(p->stmt, 3);

    /* Assuming that p2 is an instruction address, set variable p2op to the
    ** index of that instruction in the indents[] array. p2 and p2op may be
    ** different if the current instruction is part of a sub-program generated
    ** by an SQL trigger or foreign key.  */
    int p2op = (p2 + (i_op-i_addr));

    /* Grow the indents array as required */
    if( i_op>=n_alloc ){
      n_alloc += 100;
      indents = (int*)dbsql_realloc64(indents, n_alloc*sizeof(int));
      ab_yield = (int*)dbsql_realloc64(ab_yield, n_alloc*sizeof(int));
      if( indents==0 || ab_yield==0 ){
        __qrf_oom(p);
        dbsql_free(indents);
        dbsql_free(ab_yield);
        return;
      }
    }

    ab_yield[i_op] = __qrf_string_in_array(op, yields);
    indents[i_op] = 0;
    n_indent = i_op+1;
    if( __qrf_string_in_array(op, nexts) && p2op>0 ){
      for(i=p2op; i<i_op; i++) indents[i] += 2;
    }
    if( __qrf_string_in_array(op, gotos) && p2op<i_op && (ab_yield[p2op] || p1) ){
      for(i=p2op; i<i_op; i++) indents[i] += 2;
    }
  }
  dbsql_free(ab_yield);

  /* Second pass.  Actually generate output */
  dbsql_reset(p->stmt);
  if( p->i_err==DBSQL_OK ){
    static const int explain_widths[] = {4,       13, 4, 4, 4, 13, 2, 13};
    static const int explain_maps[] =   {0,       1,  2, 3, 4, 5,  6, 7 };
    static const int scan_exp_widths[] = {4,15, 6, 13, 4, 4, 4, 13, 2, 13};
    static const int scan_exp_maps[] =   {0, 9, 8, 1,  2, 3, 4, 5,  6, 7 };
    const int *widths = explain_widths;
    const int *maps = explain_maps;
    int n_width = sizeof(explain_widths)/sizeof(int);
    int i_indent = 1;
    int n_arg = p->n_col;
    if( p->spec.style==QRF_STYLE_StatsVm ){
      widths = scan_exp_widths;
      maps = scan_exp_maps;
      n_width = sizeof(scan_exp_widths)/sizeof(int);
      i_indent = 3;
    }
    if( n_arg>n_width ) n_arg = n_width;

    for(i_op=0; dbsql_step(p->stmt)==DBSQL_ROW && !p->i_err; i_op++){
      /* If this is the first row seen, print out the headers */
      if( i_op==0 ){
        for(i=0; i<n_arg; i++){
          const char *col = dbsql_column_name(p->stmt, maps[i]);
          __qrf_width_print(p,p->out, widths[i], col);
          if( i==n_arg-1 ){
            dbsql_str_append(p->out, "\n", 1);
          }else{
            dbsql_str_append(p->out, "  ", 2);
          }
        }
        for(i=0; i<n_arg; i++){
          dbsql_str_appendf(p->out, "%.*c", widths[i], '-');
          if( i==n_arg-1 ){
            dbsql_str_append(p->out, "\n", 1);
          }else{
            dbsql_str_append(p->out, "  ", 2);
          }
        }
      }
  
      for(i=0; i<n_arg; i++){
        const char *sep = "  ";
        int w = widths[i];
        const char *val = (const char*)dbsql_column_text(p->stmt, maps[i]);
        int len;
        if( i==n_arg-1 ) w = 0;
        if( val==0 ) val = "";
        len = (int)dbsql_qrf_wcswidth(val);
        if( len>w ){
          w = len;
          sep = " ";
        }
        if( i==i_indent && indents && i_op<n_indent ){
          dbsql_str_appendchar(p->out, indents[i_op], ' ');
        }
        __qrf_width_print(p, p->out, w, val);
        if( i==n_arg-1 ){
          dbsql_str_append(p->out, "\n", 1);
        }else{
          dbsql_str_appendall(p->out, sep);
        }
      }
      p->n_row++;
    }
    __qrf_write(p);
  }
  dbsql_free(indents);
}

/*
** Do a "scanstatus vm" style EXPLAIN listing on p->stmt.
**
** p->stmt is probably not an EXPLAIN query.  Instead, construct a
** new query that is a bytecode() rendering of p->stmt with extra
** columns for the "scanstatus vm" outputs, and run the results of
** that new query through the normal EXPLAIN formatting.
*/
static void __qrf_scan_status_vm(qrf_t *p){
  dbsql_stmt_t *orig_stmt = p->stmt;
  dbsql_stmt_t *explain;
  int rc;
  static const char *sql =
      "  SELECT addr, opcode, p1, p2, p3, p4, p5, comment, nexec,"
      "   format('% 6s (%.2f%%)',"
      "      CASE WHEN ncycle<100_000 THEN ncycle || ' '"
      "         WHEN ncycle<100_000_000 THEN (ncycle/1_000) || 'K'"
      "         WHEN ncycle<100_000_000_000 THEN (ncycle/1_000_000) || 'M'"
      "         ELSE (ncycle/1000_000_000) || 'G' END,"
      "       ncycle*100.0/(sum(ncycle) OVER ())"
      "   )  AS cycles"
      "   FROM bytecode(?1)";
  rc = dbsql_prepare_v2(p->db, sql, -1, &explain, 0);
  if( rc ){
    __qrf_error(p, rc, "%s", dbsql_errmsg(p->db));
    dbsql_finalize(explain);
    return;
  }
  dbsql_bind_pointer(explain, 1, orig_stmt, "stmt-pointer", 0);
  p->stmt = explain;
  p->n_col = 10;
  __qrf_explain(p);
  dbsql_finalize(explain);
  p->stmt = orig_stmt;
}

/*
** Attempt to determine if identifier name needs to be quoted, either
** because it contains non-alphanumeric characters, or because it is an
** dbsql keyword.  Be conservative in this estimate:  When in doubt assume
** that quoting is required.
**
** Return 1 if quoting is required.  Return 0 if no quoting is required.
*/

static int qrf_need_quote(const char *name){
  int i;
  const unsigned char *z = (const unsigned char*)name;
  if( z==0 ) return 1;
  if( !QRF_ALPHA(z[0]) ) return 1;
  for(i=0; z[i]; i++){
    if( !QRF_ALNUM(z[i]) ) return 1;
  }
  return dbsql_keyword_check(name, i)!=0;
}

/*
** Helper function for QRF_STYLE_Json and QRF_STYLE_JObject.
** The initial "{" for a JSON object that will contain row content
** has been output.  Now output all the content.
*/
static void __qrf_one_json_row(qrf_t *p){
  int i, n_item; 
  for(n_item=i=0; i<p->n_col; i++){
    const char *c_name;
    c_name = dbsql_column_name(p->stmt, i);
    if( n_item>0 ) dbsql_str_append(p->out, ",", 1);
    n_item++;
    __qrf_encode_text(p, p->out, c_name);
    dbsql_str_append(p->out, ":", 1);
    __qrf_render_value(p, p->out, i);
  }
  __qrf_write(p);
}

/*
** Render a single row of output for non-columnar styles - any
** style that lets us render row by row as the content is received
** from the query.
*/
static void __qrf_one_simple_row(qrf_t *p){
  int i;
  switch( p->spec.style ){
    case QRF_STYLE_Off:
    case QRF_STYLE_Count: {
      /* No-op */
      break;
    }
    case QRF_STYLE_Json: {
      if( p->n_row==0 ){
        dbsql_str_append(p->out, "[{", 2);
      }else{
        dbsql_str_append(p->out, "},\n{", 4);
      }
      __qrf_one_json_row(p);
      break;
    }
    case QRF_STYLE_JObject: {
      if( p->n_row==0 ){
        dbsql_str_append(p->out, "{", 1);
      }else{
        dbsql_str_append(p->out, "}\n{", 3);
      }
      __qrf_one_json_row(p);
      break;
    }
    case QRF_STYLE_Html: {
      if( p->n_row==0 && p->spec.titles==QRF_Yes ){
        dbsql_str_append(p->out, "<TR>", 4);
        for(i=0; i<p->n_col; i++){
          const char *c_name = dbsql_column_name(p->stmt, i);
          dbsql_str_append(p->out, "\n<TH>", 5);
          __qrf_encode_text(p, p->out, c_name);
        }
        dbsql_str_append(p->out, "\n</TR>\n", 7);
      }
      dbsql_str_append(p->out, "<TR>", 4);
      for(i=0; i<p->n_col; i++){
        dbsql_str_append(p->out, "\n<TD>", 5);
        __qrf_render_value(p, p->out, i);
      }
      dbsql_str_append(p->out, "\n</TR>\n", 7);
      __qrf_write(p);
      break;
    }
    case QRF_STYLE_Insert: {
      unsigned int mx_ins = p->spec.n_multi_insert;
      int sz_start = dbsql_str_length(p->out);
      if( p->u.n_ins==0 || p->u.n_ins>=mx_ins ){
        if( p->u.n_ins ){
          dbsql_str_append(p->out, ";\n", 2);
          p->u.n_ins = 0;
        }
        if( qrf_need_quote(p->spec.table_name) ){
          dbsql_str_appendf(p->out,"INSERT INTO \"%w\"",p->spec.table_name);
        }else{
          dbsql_str_appendf(p->out,"INSERT INTO %s",p->spec.table_name);
        }
        if( p->spec.titles==QRF_Yes ){
          for(i=0; i<p->n_col; i++){
            const char *c_name = dbsql_column_name(p->stmt, i);
            if( qrf_need_quote(c_name) ){
              dbsql_str_appendf(p->out, "%c\"%w\"",
                                  i==0 ? '(' : ',', c_name);
            }else{
              dbsql_str_appendf(p->out, "%c%s",
                                  i==0 ? '(' : ',', c_name);
            }
          }
          dbsql_str_append(p->out, ")", 1);
        }
        dbsql_str_append(p->out," VALUES(", 8);
      }else{
        dbsql_str_append(p->out,",\n  (", 5);
      }
      for(i=0; i<p->n_col; i++){
        if( i>0 ) dbsql_str_append(p->out, ",", 1);
        __qrf_render_value(p, p->out, i);
      }
      p->u.n_ins += dbsql_str_length(p->out) + 2 - sz_start;
      if( p->u.n_ins>=mx_ins ){
        dbsql_str_append(p->out, ");\n", 3);
        p->u.n_ins = 0;
      }else{
        dbsql_str_append(p->out, ")", 1);
      }
      __qrf_write(p);
      break;
    }
    case QRF_STYLE_Line: {
      dbsql_str_t *val;
      int mx_w;
      int ww;
      int n_sep;
      if( p->u.s_line.cols==0 ){
        p->u.s_line.cols = dbsql_malloc64( p->n_col*sizeof(char*) );
        if( p->u.s_line.cols==0 ){
          __qrf_oom(p);
          break;
        }
        p->u.s_line.mx_col_wth = 0;
        for(i=0; i<p->n_col; i++){
          int sz;
          const char *c_name = dbsql_column_name(p->stmt, i);
          if( c_name==0 ) c_name = "unknown";
          p->u.s_line.cols[i] = dbsql_mprintf("%s", c_name);
          if( p->spec.n_title_limit>0 ){
            (void)__qrf_title_limit(p->u.s_line.cols[i], p->spec.n_title_limit);
          }
          sz = (int)dbsql_qrf_wcswidth(p->u.s_line.cols[i]);
          if( sz > p->u.s_line.mx_col_wth ) p->u.s_line.mx_col_wth = sz;
        }
      }
      if( p->n_row ) dbsql_str_append(p->out, "\n", 1);
      val = dbsql_str_new(p->db);
      n_sep = (int)strlen(p->spec.column_sep);
      mx_w = p->mx_width - (n_sep + p->u.s_line.mx_col_wth);
      ww = p->spec.word_wrap==QRF_Yes;
      for(i=0; i<p->n_col; i++){
        const char *z_val;
        int cnt = 0;
        __qrf_width_print(p, p->out, -p->u.s_line.mx_col_wth, p->u.s_line.cols[i]);
        dbsql_str_append(p->out, p->spec.column_sep, n_sep);
        __qrf_render_value(p, val, i);
        z_val = dbsql_str_value(val);
        if( z_val==0 ) z_val = "";
        do{
          int n_this, n_wide, i_next;
          __qrf_wrap_line(z_val, mx_w, ww, &n_this, &n_wide, &i_next);
          if( cnt ){
            dbsql_str_appendchar(p->out,p->u.s_line.mx_col_wth+n_sep,' ');
          }
          cnt++;
          if( cnt>p->mx_height ){
            z_val = "...";
            n_this = i_next = 3;
          }
          dbsql_str_append(p->out, z_val, n_this);
          dbsql_str_append(p->out, "\n", 1);
          z_val += i_next;
        }while( z_val[0] );
        dbsql_str_reset(val);
      }
      __qrf_str_err(p, val);
      dbsql_free(dbsql_str_finish(val));
      __qrf_write(p);
      break;
    }
    case QRF_STYLE_Eqp: {
      const char *eqp_line = (const char*)dbsql_column_text(p->stmt,3);
      int i_eqp_id = dbsql_column_int(p->stmt, 0);
      int i_parent_id = dbsql_column_int(p->stmt, 1);
      if( eqp_line==0 ) eqp_line = "";
      if( eqp_line[0]=='-' ) __qrf_eqp_render(p, 0);
      __qrf_eqp_append(p, i_eqp_id, i_parent_id, eqp_line);
      break;
    }
    default: {  /* QRF_STYLE_List */
      if( p->n_row==0 && p->spec.titles==QRF_Yes ){
        int saved_e_text = p->spec.text;
        p->spec.text = p->spec.title;
        for(i=0; i<p->n_col; i++){
          const char *c_name = dbsql_column_name(p->stmt, i);
          if( i>0 ) dbsql_str_appendall(p->out, p->spec.column_sep);
          __qrf_encode_text(p, p->out, c_name);
        }
        dbsql_str_appendall(p->out, p->spec.row_sep);
        __qrf_write(p);
        p->spec.text = saved_e_text;
      }
      for(i=0; i<p->n_col; i++){
        if( i>0 ) dbsql_str_appendall(p->out, p->spec.column_sep);
        __qrf_render_value(p, p->out, i);
      }
      dbsql_str_appendall(p->out, p->spec.row_sep);
      __qrf_write(p);
      break;
    }
  }
  p->n_row++;
}

/*
** Initialize the internal qrf_t object.
*/
static void __qrf_initialize(
  qrf_t *p,                        /* State object to be initialized */
  dbsql_stmt_t *stmt,           /* Query whose output to be formatted */
  const dbsql_qrf_spec *spec, /* Format specification */
  char **err_p                   /* Write errors here */
){
  size_t sz;                     /* Size of spec[], based on spec->i_version */
  memset(p, 0, sizeof(*p));
  p->err_p = err_p;
  if( spec->i_version>1 ){
    __qrf_error(p, DBSQL_ERROR,
       "unusable dbsql_qrf_spec.iVersion (%d)",
       spec->i_version);
    return;
  }
  p->stmt = stmt;
  p->db = dbsql_db_handle(stmt);
  p->out = dbsql_str_new(p->db);
  if( p->out==0 ){
    __qrf_oom(p);
    return;
  }
  p->i_err = DBSQL_OK;
  p->n_col = dbsql_column_count(p->stmt);
  p->n_row = 0;
  sz = sizeof(dbsql_qrf_spec);
  memcpy(&p->spec, spec, sz);
  if( p->spec.null==0 ) p->spec.null = "";
  p->mx_width = p->spec.n_screen_width;
  if( p->mx_width<=0 ) p->mx_width = QRF_MAX_WIDTH;
  p->mx_height = p->spec.n_line_limit;
  if( p->mx_height<=0 ) p->mx_height = 2147483647;
  if( p->spec.style>QRF_STYLE_Table ) p->spec.style = QRF_Auto;
  if( p->spec.esc>QRF_ESC_Symbol ) p->spec.esc = QRF_Auto;
  if( p->spec.text>QRF_TEXT_Relaxed ) p->spec.text = QRF_Auto;
  if( p->spec.title>QRF_TEXT_Relaxed ) p->spec.title = QRF_Auto;
  if( p->spec.blob>QRF_BLOB_Size ) p->spec.blob = QRF_Auto;
qrf_reinit:
  switch( p->spec.style ){
    case QRF_Auto: {
      switch( dbsql_stmt_isexplain(stmt) ){
        case 0:  p->spec.style = QRF_STYLE_Box;      break;
        case 1:  p->spec.style = QRF_STYLE_Explain;  break;
        default: p->spec.style = QRF_STYLE_Eqp;      break;
      }
      goto qrf_reinit;
    }
    case QRF_STYLE_List: {
      if( p->spec.column_sep==0 ) p->spec.column_sep = "|";
      if( p->spec.row_sep==0 ) p->spec.row_sep = "\n";
      break;
    }
    case QRF_STYLE_JObject:
    case QRF_STYLE_Json: {
      p->spec.text = QRF_TEXT_Json;
      p->spec.null = "null";
      break;
    }
    case QRF_STYLE_Html: {
      p->spec.text = QRF_TEXT_Html;
      p->spec.null = "null";
      break;
    }
    case QRF_STYLE_Insert: {
      p->spec.text = QRF_TEXT_Sql;
      p->spec.null = "NULL";
      if( p->spec.table_name==0 || p->spec.table_name[0]==0 ){
        p->spec.table_name = "tab";
      }
      p->u.n_ins = 0;
      break;
    }
    case QRF_STYLE_Line: {
      if( p->spec.column_sep==0 ){
        p->spec.column_sep = ": ";
      }
      break;
    }
    case QRF_STYLE_Csv: {
      p->spec.style = QRF_STYLE_List;
      p->spec.text = QRF_TEXT_Csv;
      p->spec.column_sep = ",";
      p->spec.row_sep = "\r\n";
      p->spec.null = "";
      break;
    }
    case QRF_STYLE_Quote: {
      p->spec.text = QRF_TEXT_Sql;
      p->spec.null = "NULL";
      p->spec.column_sep = ",";
      p->spec.row_sep = "\n";
      break;
    }
    case QRF_STYLE_Eqp: {
      int exp_mode = dbsql_stmt_isexplain(p->stmt);
      if( exp_mode!=2 ){
        dbsql_stmt_explain(p->stmt, 2);
        p->exp_mode = exp_mode+1;
      }
      break;
    }
    case QRF_STYLE_Explain: {
      int exp_mode = dbsql_stmt_isexplain(p->stmt);
      if( exp_mode!=1 ){
        dbsql_stmt_explain(p->stmt, 1);
        p->exp_mode = exp_mode+1;
      }
      break;
    }
  }
  if( p->spec.esc==QRF_Auto ){
    p->spec.esc = QRF_ESC_Ascii;
  }
  if( p->spec.text==QRF_Auto ){
    p->spec.text = QRF_TEXT_Plain;
  }
  if( p->spec.title==QRF_Auto ){
    switch( p->spec.style ){
      case QRF_STYLE_Box:
      case QRF_STYLE_Column:
      case QRF_STYLE_Table:
        p->spec.title = QRF_TEXT_Plain;
        break;
      default:
        p->spec.title = p->spec.text;
        break;
    }
  }
  if( p->spec.blob==QRF_Auto ){
    switch( p->spec.text ){
      case QRF_TEXT_Sql:  p->spec.blob = QRF_BLOB_Sql;  break;
      case QRF_TEXT_Csv:  p->spec.blob = QRF_BLOB_Tcl;  break;
      case QRF_TEXT_Tcl:  p->spec.blob = QRF_BLOB_Tcl;  break;
      case QRF_TEXT_Json: p->spec.blob = QRF_BLOB_Json; break;
      default:            p->spec.blob = QRF_BLOB_Text; break;
    }
  }
  if( p->spec.titles==QRF_Auto ){
    switch( p->spec.style ){
      case QRF_STYLE_Box:
      case QRF_STYLE_Csv:
      case QRF_STYLE_Column:
      case QRF_STYLE_Table:
      case QRF_STYLE_Markdown:
        p->spec.titles = QRF_Yes;
        break;
      default:
        p->spec.titles = QRF_No;
        break;
    }
  }
  if( p->spec.word_wrap==QRF_Auto ){
    p->spec.word_wrap = QRF_Yes;
  }
  if( p->spec.text_jsonb==QRF_Auto ){
    p->spec.text_jsonb = QRF_No;
  }
  if( p->spec.column_sep==0 ) p->spec.column_sep = ",";
  if( p->spec.row_sep==0 ) p->spec.row_sep = "\n";
}

/*
** Finish rendering the results
*/
static void __qrf_finalize(qrf_t *p){
  switch( p->spec.style ){
    case QRF_STYLE_Count: {
      dbsql_str_appendf(p->out, "%lld\n", p->n_row);
      break;
    }
    case QRF_STYLE_Json: {
      if( p->n_row>0 ){
        dbsql_str_append(p->out, "}]\n", 3);
      }
      break;
    }
    case QRF_STYLE_JObject: {
      if( p->n_row>0 ){
        dbsql_str_append(p->out, "}\n", 2);
      }
      break;
    }
    case QRF_STYLE_Insert: {
      if( p->u.n_ins ){
        dbsql_str_append(p->out, ";\n", 2);
      }
      break;
    }
    case QRF_STYLE_Line: {
      if( p->u.s_line.cols ){
        int i;
        for(i=0; i<p->n_col; i++) dbsql_free(p->u.s_line.cols[i]);
        dbsql_free(p->u.s_line.cols);
      }
      break;
    }
    case QRF_STYLE_Stats:
    case QRF_STYLE_StatsEst: {
      int64_t n_cycle = 0;
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
      dbsql_stmt_scanstatus_v2(p->stmt, -1, DBSQL_SCANSTAT_NCYCLE,
                                 DBSQL_SCANSTAT_COMPLEX, (void*)&n_cycle);
#endif
      __qrf_eqp_render(p, n_cycle);
      break;
    }
    case QRF_STYLE_Eqp: {
      __qrf_eqp_render(p, 0);
      break;
    }
  }
  __qrf_write(p);
  __qrf_str_err(p, p->out);
  if( p->spec.output ){
    if( p->spec.output[0] ){
      int64_t n, sz;
      char *combined;
      sz = strlen(p->spec.output[0]);
      n = dbsql_str_length(p->out);
      combined = dbsql_realloc64(p->spec.output[0], sz+n+1);
      if( combined==0 ){
        dbsql_free(p->spec.output[0]);
        p->spec.output[0] = 0;
        __qrf_oom(p);
      }else{
        p->spec.output[0] = combined;
        memcpy(combined+sz, dbsql_str_value(p->out), n+1);
      }
      dbsql_free(dbsql_str_finish(p->out));
    }else{
      p->spec.output[0] = dbsql_str_finish(p->out);
    }
  }else if( p->out ){
    dbsql_free(dbsql_str_finish(p->out));
  }
  if( p->exp_mode>0 ){
    dbsql_stmt_explain(p->stmt, p->exp_mode-1);
  }
  if( p->actual_width ){
    dbsql_free(p->actual_width);
  }
  if( p->j_trans ){
    DBSQL *db = dbsql_db_handle(p->j_trans);
    dbsql_finalize(p->j_trans);
    dbsql_close(db);
  }
}

/*
** Run the prepared statement stmt and format the results according
** to the specification provided in spec.  Return an error code.
** If err_p is not NULL and if an error occurs, write an error message
** into *err_p.
*/
int dbsql_format_query_result(
  dbsql_stmt_t *stmt,                 /* Statement to evaluate */
  const dbsql_qrf_spec *spec,       /* Format specification */
  char **err_p                         /* Write error message here */
){
  qrf_t qrf;         /* The new qrf_t being created */

  if( stmt==0 ) return DBSQL_OK;       /* No-op */
  if( spec==0 ) return DBSQL_MISUSE;
  __qrf_initialize(&qrf, stmt, spec, err_p);
  switch( qrf.spec.style ){
    case QRF_STYLE_Box:
    case QRF_STYLE_Column:
    case QRF_STYLE_Markdown: 
    case QRF_STYLE_Table: {
      /* Columnar modes require that the entire query be evaluated and the
      ** results stored in memory, so that we can compute column widths */
      __qrf_columnar(&qrf);
      break;
    }
    case QRF_STYLE_Explain: {
      __qrf_explain(&qrf);
      break;
    }
    case QRF_STYLE_StatsVm: {
      __qrf_scan_status_vm(&qrf);
      break;
    }
    case QRF_STYLE_Stats:
    case QRF_STYLE_StatsEst: {
      __qrf_eqp_stats(&qrf);
      break;
    }
    default: {
      /* Non-columnar modes where the output can occur after each row
      ** of result is received */
      while( qrf.i_err==DBSQL_OK && dbsql_step(stmt)==DBSQL_ROW ){
        __qrf_one_simple_row(&qrf);
      }
      break;
    }
  }
  __qrf_reset_stmt(&qrf);
  __qrf_finalize(&qrf);
  return qrf.i_err;
}
