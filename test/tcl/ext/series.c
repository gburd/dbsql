/*
** 2015-08-18, 2023-04-28
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
** This file demonstrates how to create a table-valued-function using
** a virtual table.  This demo implements the generate_series() function
** which gives the same results as the eponymous function in __postgre_sql,
** within the limitation that its arguments are signed 64-bit integers.
**
** Considering its equivalents to generate_series(start,stop,step): A
** value V[n] sequence is produced for integer n ascending from 0 where
**  ( V[n] == start + n * step  &&  sgn(V[n] - stop) * sgn(step) >= 0 )
** for each produced value (independent of production time ordering.)
**
** All parameters must be either integer or convertable to integer.
** The start parameter is required.
** The stop parameter defaults to (1<<32)-1 (aka 4294967295 or 0xffffffff)
** The step parameter defaults to 1 and 0 is treated as 1.
**
** Examples:
**
**      SELECT * FROM generate_series(0,100,5);
**
** The query above returns integers from 0 through 100 counting by steps
** of 5.  In other words, 0, 5, 10, 15, ..., 90, 95, 100.  There are a total
** of 21 rows.
**
**      SELECT * FROM generate_series(0,100);
**
** Integers from 0 through 100 with a step size of 1.  101 rows.
**
**      SELECT * FROM generate_series(20) LIMIT 10;
**
** Integers 20 through 29.  10 rows.
**
**      SELECT * FROM generate_series(0,-100,-5);
**
** Integers 0 -5 -10 ... -100.  21 rows.
**
**      SELECT * FROM generate_series(0,-1);
**
** Empty sequence.
**
** HOW IT WORKS
**
** The generate_series "function" is really a virtual table with the
** following schema:
**
**     CREATE TABLE generate_series(
**       value,
**       start HIDDEN,
**       stop HIDDEN,
**       step HIDDEN
**     );
**
** The virtual table also has a rowid which is an alias for the value.
**
** Function arguments in queries against this virtual table are translated
** into equality constraints against successive hidden columns.  In other
** words, the following pairs of queries are equivalent to each other:
**
**    SELECT * FROM generate_series(0,100,5);
**    SELECT * FROM generate_series WHERE start=0 AND stop=100 AND step=5;
**
**    SELECT * FROM generate_series(0,100);
**    SELECT * FROM generate_series WHERE start=0 AND stop=100;
**
**    SELECT * FROM generate_series(20) LIMIT 10;
**    SELECT * FROM generate_series WHERE start=20 LIMIT 10;
**
** The generate_series virtual table implementation leaves the create method
** set to NULL.  This means that it is not possible to do a CREATE VIRTUAL
** TABLE command with "generate_series" as the USING argument.  Instead, there
** is a single generate_series virtual table that is always available without
** having to be created first.
**
** The best_index method looks for equality constraints against the hidden
** start, stop, and step columns, and if present, it uses those constraints
** to bound the sequence of generated values.  If the equality constraints
** are missing, it uses 0 for start, 4294967295 for stop, and 1 for step.
** best_index returns a small cost when both start and stop are available,
** and a very large cost if either start or stop are unavailable.  This
** encourages the query planner to order joins such that the bounds of the
** series are well-defined.
**
** Update on 2024-08-22:
** best_index now also looks for equality and inequality constraints against
** the value column and uses those constraints as additional bounds against
** the sequence range.  Thus, a query like this:
**
**     SELECT value FROM generate_series($SA,$EA)
**      WHERE value BETWEEN $SB AND $EB;
**
** Is logically the same as:
**
**     SELECT value FROM generate_series(max($SA,$SB),min($EA,$EB));
**
** Constraints on the value column can server as substitutes for constraints
** on the hidden start and stop columns.  So, the following two queries
** are equivalent:
**
**     SELECT value FROM generate_series($S,$E);
**     SELECT value FROM generate_series WHERE value BETWEEN $S and $E;
**
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/* series_cursor is a subclass of dbsql_vtab_cursor_t which will
** serve as the underlying representation of a cursor that scans
** over rows of the result.
**
** i_o_base, i_o_term, and i_o_step are the original values of the
** start=, stop=, and step= constraints on the query.  These are
** the values reported by the start, stop, and step columns of the
** virtual table.
**
** i_base, i_term, i_step, and descp are the actual values used to generate
** the sequence.  These might be different from the i_oxxxx values.
** For example in
**
**   SELECT value FROM generate_series(1,11,2)
**    WHERE value BETWEEN 4 AND 8;
**
** The i_o_base is 1, but the i_base is 5.  i_o_term is 11 but i_term is 7.
** Another example:
**
**   SELECT value FROM generate_series(1,15,3) ORDER BY value DESC;
**
** The cursor initialization for the above query is:
**
**   i_o_base = 1        i_base = 13
**   i_o_term = 15       i_term = 1
**   i_o_step = 3        i_step = 3      desc = 1
**
** The actual step size is unsigned so that can have a value of
** +9223372036854775808 which is needed for querys like this:
**
**   SELECT value
**     FROM generate_series(9223372036854775807,
**                          -9223372036854775808,
**                          -9223372036854775808)
**    ORDER BY value ASC;
**
** The setup for the previous query will be:
**
**   i_o_base = 9223372036854775807    i_base = -1
**   i_o_term = -9223372036854775808   i_term = 9223372036854775807
**   i_o_step = -9223372036854775808   i_step = 9223372036854775808  desc = 0
*/
typedef unsigned char u_int8_t;
typedef struct series_cursor series_cursor;
struct series_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_o_base;      /* Original starting value ("start") */
  int64_t i_o_term;      /* Original terminal value ("stop") */
  int64_t i_o_step;      /* Original step value */
  int64_t i_base;       /* Starting value to actually use */
  int64_t i_term;       /* Terminal value to actually use */
  u_int64_t i_step;      /* The step size */
  int64_t i_value;      /* Current value */
  u_int8_t desc;                  /* i_step is really negative */
  u_int8_t done;                  /* True if stepped past last element */
};

/*
** Computed the difference between two 64-bit signed integers using a
** convoluted computation designed to work around the silly restriction
** against signed integer overflow in C.
*/
static u_int64_t span64(int64_t a, int64_t b){
  assert( a>=b );
  return (*(u_int64_t*)&a) - (*(u_int64_t*)&b);
}  

/*
** Add or substract an unsigned 64-bit integer from a signed 64-bit integer
** and return the new signed 64-bit integer.
*/
static int64_t add64(int64_t a, u_int64_t b){
  u_int64_t x = *(u_int64_t*)&a;
  x += b;
  return *(int64_t*)&x;
}
static int64_t sub64(int64_t a, u_int64_t b){
  u_int64_t x = *(u_int64_t*)&a;
  x -= b;
  return *(int64_t*)&x;
}

/*
** The __series_connect() method is invoked to create a new
** series_vtab that describes the generate_series virtual table.
**
** Think of this routine as the constructor for series_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the series_vtab object and initialize all fields.
**
**    (2) Tell dbsql (via the dbsql_declare_vtab() interface) what the
**        result set of queries against generate_series will look like.
*/
static int __series_connect(
  DBSQL *db,
  void *unused,
  int argc_unused, const char *const*argv_unused,
  dbsql_vtab_t **vtab,
  char **err_unused
){
  dbsql_vtab_t *p_new;
  int rc;

/* column_t numbers */
#define SERIES_COLUMN_ROWID (-1)
#define SERIES_COLUMN_VALUE 0
#define SERIES_COLUMN_START 1
#define SERIES_COLUMN_STOP  2
#define SERIES_COLUMN_STEP  3

  (void)unused;
  (void)argc_unused;
  (void)argv_unused;
  (void)err_unused;
  rc = dbsql_declare_vtab(db,
     "CREATE TABLE x(value,start hidden,stop hidden,step hidden)");
  if( rc==DBSQL_OK ){
    p_new = *vtab = dbsql_malloc64( sizeof(*p_new) );
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
    dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
  }
  return rc;
}

/*
** This method is the destructor for series_cursor objects.
*/
static int __series_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Constructor for a new series_cursor object.
*/
static int __series_open(dbsql_vtab_t *unused, dbsql_vtab_cursor_t **cursor){
  series_cursor *cur;
  (void)unused;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Destructor for a series_cursor.
*/
static int __series_close(dbsql_vtab_cursor_t *cur){
  dbsql_free(cur);
  return DBSQL_OK;
}


/*
** Advance a series_cursor to its next row of output.
*/
static int __series_next(dbsql_vtab_cursor_t *cur){
  series_cursor *p_cur = (series_cursor*)cur;
  if( p_cur->i_value==p_cur->i_term ){
    p_cur->done = 1;
  }else if( p_cur->desc ){
    p_cur->i_value = sub64(p_cur->i_value, p_cur->i_step);
    assert( p_cur->i_value>=p_cur->i_term );
  }else{
    p_cur->i_value = add64(p_cur->i_value, p_cur->i_step);
    assert( p_cur->i_value<=p_cur->i_term );
  }
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the series_cursor
** is currently pointing.
*/
static int __series_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  series_cursor *p_cur = (series_cursor*)cur;
  int64_t x = 0;
  switch( i ){
    case SERIES_COLUMN_START:  x = p_cur->i_o_base;     break;
    case SERIES_COLUMN_STOP:   x = p_cur->i_o_term;     break;
    case SERIES_COLUMN_STEP:   x = p_cur->i_o_step;     break;
    default:                   x = p_cur->i_value;     break;
  }
  dbsql_result_int64(ctx, x);
  return DBSQL_OK;
}

#ifndef LARGEST_UINT64
#define LARGEST_INT64  ((int64_t)0x7fffffffffffffffLL)
#define LARGEST_UINT64 ((u_int64_t)0xffffffffffffffffULL)
#define SMALLEST_INT64 ((int64_t)0x8000000000000000LL)
#endif

/*
** The rowid is the same as the value.
*/
static int __series_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  series_cursor *p_cur = (series_cursor*)cur;
  *rowid = p_cur->i_value;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __series_eof(dbsql_vtab_cursor_t *cur){
  series_cursor *p_cur = (series_cursor*)cur;
  return p_cur->done;
}

/* True to cause run-time checking of the start=, stop=, and/or step=
** parameters.  The only reason to do this is for testing the
** constraint checking logic for virtual tables in the dbsql core.
*/
#ifndef DBSQL_SERIES_CONSTRAINT_VERIFY
# define DBSQL_SERIES_CONSTRAINT_VERIFY 0
#endif

/*
** Return the number of steps between cur->i_base and cur->i_term if
** the step width is cur->i_step.
*/
static u_int64_t __series_steps(series_cursor *cur){
  if( cur->desc ){
    assert( cur->i_base >= cur->i_term );
    return span64(cur->i_base, cur->i_term)/cur->i_step;
  }else{
    assert( cur->i_base <= cur->i_term );
    return span64(cur->i_term, cur->i_base)/cur->i_step;
  }
}

#if defined(DBSQL_ENABLE_MATH_FUNCTIONS) || defined(_WIN32)
/*
** Case 1 (the most common case):
** The standard math library is available so use ceil() and floor() from there.
*/
static double __series_ceil(double r){ return ceil(r); }
static double __series_floor(double r){ return floor(r); }
#elif defined(__GNUC__) && !defined(DBSQL_DISABLE_INTRINSIC)
/*
** Case 2 (2nd most common): Use GCC/Clang builtins
*/
static double __series_ceil(double r){ return __builtin_ceil(r); }
static double __series_floor(double r){ return __builtin_floor(r); }
#else
/*
** Case 3 (rarely happens): Use home-grown ceil() and floor() routines.
*/
static double __series_ceil(double r){
  int64_t x;
  if( r!=r ) return r;
  if( r<=(-4503599627370496.0) ) return r;
  if( r>=(+4503599627370496.0) ) return r;
  x = (int64_t)r;
  if( r==(double)x ) return r;
  if( r>(double)x ) x++;
  return (double)x;
}
static double __series_floor(double r){
  int64_t x;
  if( r!=r ) return r;
  if( r<=(-4503599627370496.0) ) return r;
  if( r>=(+4503599627370496.0) ) return r;
  x = (int64_t)r;
  if( r==(double)x ) return r;
  if( r<(double)x ) x--;
  return (double)x;
}
#endif

/* Convert a floating point value to its closest integer.  Do so in
** a way that avoids 'outside the range of representable values' warnings
** from UBSAN.
*/
static int64_t __series_real_to_i64(double r){
  if( r<-9223372036854774784.0 ) return SMALLEST_INT64;
  if( r>+9223372036854774784.0 ) return LARGEST_INT64;
  return (int64_t)r;
}

/*
** This method is called to "rewind" the series_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to __series_column() or __series_rowid() or
** __series_eof().
**
** The query plan selected by __series_best_index is passed in the idx_num
** parameter.  (idx_str is not used in this implementation.)  idx_num
** is a bitmask showing which constraints are available:
**
**   0x0001:    start=VALUE
**   0x0002:    stop=VALUE
**   0x0004:    step=VALUE
**   0x0008:    descending order
**   0x0010:    ascending order
**   0x0020:    LIMIT  VALUE
**   0x0040:    OFFSET  VALUE
**   0x0080:    value=VALUE
**   0x0100:    value>=VALUE
**   0x0200:    value>VALUE
**   0x1000:    value<=VALUE
**   0x2000:    value<VALUE
**
** This routine should initialize the cursor and position it so that it
** is pointing at the first row, or pointing off the end of the table
** (so that __series_eof() will return true) if the table is empty.
*/
static int __series_filter(
  dbsql_vtab_cursor_t *vtab_cursor,
  int idx_num, const char *idx_str_unused,
  int argc, dbsql_value_t **argv
){
  series_cursor *cur = (series_cursor *)vtab_cursor;
  int i_arg = 0;                         /* Arguments used so far */
  int i;                                /* Loop counter */
  int64_t i_min = SMALLEST_INT64;  /* Smallest allowed output value */
  int64_t i_max = LARGEST_INT64;   /* Largest allowed output value */
  int64_t i_limit = 0;             /* if >0, the value of the LIMIT */
  int64_t i_offset = 0;            /* if >0, the value of the OFFSET */

  (void)idx_str_unused;

  /* If any constraints have a NULL value, then return no rows.
  ** See ticket https://sqlite.org/src/info/fac496b61722daf2
  */
  for(i=0; i<argc; i++){
    if( dbsql_value_type(argv[i])==DBSQL_NULL ){
      goto series_no_rows;
    }
  }

  /* Capture the three HIDDEN parameters to the virtual table and insert
  ** default values for any parameters that are omitted.
  */
  if( idx_num & 0x01 ){
    cur->i_o_base = dbsql_value_int64(argv[i_arg++]);
  }else{
    cur->i_o_base = 0;
  }
  if( idx_num & 0x02 ){
    cur->i_o_term = dbsql_value_int64(argv[i_arg++]);
  }else{
    cur->i_o_term = 0xffffffff;
  }
  if( idx_num & 0x04 ){
    cur->i_o_step = dbsql_value_int64(argv[i_arg++]);
    if( cur->i_o_step==0 ) cur->i_o_step = 1;
  }else{
    cur->i_o_step = 1;
  }

  /* If there are constraints on the value column but there are
  ** no constraints on  the start, stop, and step columns, then
  ** initialize the default range to be the entire range of 64-bit signed
  ** integers.  This range will contracted by the value column constraints
  ** further below.
  */
  if( (idx_num & 0x05)==0 && (idx_num & 0x0380)!=0 ){
    cur->i_o_base = SMALLEST_INT64;
  }
  if( (idx_num & 0x06)==0 && (idx_num & 0x3080)!=0 ){
    cur->i_o_term = LARGEST_INT64;
  }
  cur->i_base = cur->i_o_base;
  cur->i_term = cur->i_o_term;
  if( cur->i_o_step>0 ){  
    cur->i_step = cur->i_o_step;
  }else if( cur->i_o_step>SMALLEST_INT64 ){
    cur->i_step = -cur->i_o_step;
  }else{
    cur->i_step = LARGEST_INT64;
    cur->i_step++;
  }
  cur->desc = cur->i_o_step<0;
  if( cur->desc==0 && cur->i_base>cur->i_term ){
    goto series_no_rows;
  }
  if( cur->desc!=0 && cur->i_base<cur->i_term ){
    goto series_no_rows;
  }

  /* Extract the LIMIT and OFFSET values, but do not apply them yet.
  ** The range must first be constrained by the limits on value.
  */
  if( idx_num & 0x20 ){
    i_limit = dbsql_value_int64(argv[i_arg++]);
    if( idx_num & 0x40 ){
      i_offset = dbsql_value_int64(argv[i_arg++]);
    }
  }

  /* Narrow the range of i_min and i_max (the minimum and maximum outputs)
  ** based on equality and inequality constraints on the "value" column.
  */
  if( idx_num & 0x3380 ){
    if( idx_num & 0x0080 ){    /* value=X */
      if( dbsql_value_numeric_type(argv[i_arg])==DBSQL_FLOAT ){
        double r = dbsql_value_double(argv[i_arg++]);
        if( r==__series_ceil(r)
         && r>=(double)SMALLEST_INT64
         && r<=(double)LARGEST_INT64
        ){
          i_min = i_max = __series_real_to_i64(r);
        }else{
          goto series_no_rows;
        }
      }else{
        i_min = i_max = dbsql_value_int64(argv[i_arg++]);
      }
    }else{
      if( idx_num & 0x0300 ){  /* value>X (0x200) or value>=X (0x100) */
        if( dbsql_value_numeric_type(argv[i_arg])==DBSQL_FLOAT ){
          double r = dbsql_value_double(argv[i_arg++]);
          if( r<=(double)SMALLEST_INT64 ){
            i_min = SMALLEST_INT64;
          }else if( r>(double)LARGEST_INT64 ){
            goto series_no_rows;
          }else{
            i_min = __series_real_to_i64(__series_ceil(r));
            if( (idx_num & 0x0200)!=0 && r==__series_ceil(r) ){
              if( i_min==LARGEST_INT64 ) goto series_no_rows;
              i_min++;
            }
          }
        }else{
          i_min = dbsql_value_int64(argv[i_arg++]);
          if( (idx_num & 0x0200)!=0 ){
            if( i_min==LARGEST_INT64 ){
              goto series_no_rows;
            }else{
              i_min++;
            }
          }
        }
      }
      if( idx_num & 0x3000 ){   /* value<X (0x2000) or value<=X (0x1000) */
        if( dbsql_value_numeric_type(argv[i_arg])==DBSQL_FLOAT ){
          double r = dbsql_value_double(argv[i_arg++]);
          if( r>=(double)LARGEST_INT64 ){
            i_max = LARGEST_INT64;
          }else if( r<=(double)SMALLEST_INT64 ){
            goto series_no_rows;
          }else{
            i_max = __series_real_to_i64(__series_floor(r));
            if( (idx_num & 0x2000)!=0 && r==__series_floor(r) ){
              if( i_max==SMALLEST_INT64 ) goto series_no_rows;
              i_max--;
            }
          }
        }else{
          i_max = dbsql_value_int64(argv[i_arg++]);
          if( idx_num & 0x2000 ){
            if( i_max==SMALLEST_INT64 ){
              goto series_no_rows;
            }else{
              i_max--;
            }
          }
        }
      }
      if( i_min>i_max ){
        goto series_no_rows;
      }
    }

    /* Try to reduce the range of values to be generated based on
    ** constraints on the "value" column.
    */
    if( cur->desc==0 ){
      if( cur->i_base<i_min ){
        u_int64_t span = span64(i_min,cur->i_base);
        cur->i_base = add64(cur->i_base, (span/cur->i_step)*cur->i_step);
        if( cur->i_base<i_min ){
          if( cur->i_base > sub64(LARGEST_INT64, cur->i_step) ){
            goto series_no_rows;
          }
          cur->i_base = add64(cur->i_base, cur->i_step);
        }
      }
      if( cur->i_term>i_max ){
        cur->i_term = i_max;
      }
    }else{
      if( cur->i_base>i_max ){
        u_int64_t span = span64(cur->i_base,i_max);
        cur->i_base = sub64(cur->i_base, (span/cur->i_step)*cur->i_step);
        if( cur->i_base>i_max ){
          if( cur->i_base < add64(SMALLEST_INT64, cur->i_step) ){
            goto series_no_rows;
          }
          cur->i_base = sub64(cur->i_base, cur->i_step);
        }
      }
      if( cur->i_term<i_min ){
        cur->i_term = i_min;
      }
    }
  }

  /* Adjust i_term so that it is exactly the last value of the series.
  */
  if( cur->desc==0 ){
    if( cur->i_base>cur->i_term ){
      goto series_no_rows;
    }
    cur->i_term = sub64(cur->i_term,
                        span64(cur->i_term,cur->i_base) % cur->i_step);
  }else{
    if( cur->i_base<cur->i_term ){
      goto series_no_rows;
    }
    cur->i_term = add64(cur->i_term,
                        span64(cur->i_base,cur->i_term) % cur->i_step);
  }

  /* Transform the series generator to output values in the requested
  ** order.
  */
  if( ((idx_num & 0x0008)!=0 && cur->desc==0)
   || ((idx_num & 0x0010)!=0 && cur->desc!=0)
  ){
    int64_t tmp = cur->i_base;
    cur->i_base = cur->i_term;
    cur->i_term = tmp;
    cur->desc = !cur->desc;
  }

  /* Apply LIMIT and OFFSET constraints, if any */
  assert( cur->i_step!=0 );
  if( idx_num & 0x20 ){
    if( i_offset>0 ){
      if( __series_steps(cur) < (u_int64_t)i_offset ){
        goto series_no_rows;
      }else if( cur->desc ){
        cur->i_base = sub64(cur->i_base, cur->i_step*i_offset);
      }else{
        cur->i_base = add64(cur->i_base, cur->i_step*i_offset);
      }
    }
    if( i_limit>=0 && __series_steps(cur) > (u_int64_t)i_limit ){
      cur->i_term = add64(cur->i_base, (i_limit - 1)*cur->i_step);
    }
  }
  cur->i_value = cur->i_base;
  cur->done = 0;
  return DBSQL_OK;

series_no_rows:
  cur->i_base = 0;
  cur->i_term = 0;
  cur->i_step = 1;
  cur->desc = 0;
  cur->done = 1;
  return DBSQL_OK;  
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the generate_series virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
**
** In this implementation idx_num is used to represent the
** query plan.  idx_str is unused.
**
** The query plan is represented by bits in idx_num:
**
**   0x0001  start = $num
**   0x0002  stop = $num
**   0x0004  step = $num
**   0x0008  output is in descending order
**   0x0010  output is in ascending order
**   0x0020  LIMIT $num
**   0x0040  OFFSET $num
**   0x0080  value = $num
**   0x0100  value >= $num
**   0x0200  value > $num
**   0x1000  value <= $num
**   0x2000  value < $num
**
** Only one of 0x0100 or 0x0200 will be returned.  Similarly, only
** one of 0x1000 or 0x2000 will be returned.  If the 0x0080 is set, then
** none of the 0xff00 bits will be set.
**
** The order of parameters passed to filter is as follows:
**
**    * The argument to start= if bit 0x0001 is in the idx_num mask
**    * The argument to stop= if bit 0x0002 is in the idx_num mask
**    * The argument to step= if bit 0x0004 is in the idx_num mask
**    * The argument to LIMIT if bit 0x0020 is in the idx_num mask
**    * The argument to OFFSET if bit 0x0040 is in the idx_num mask
**    * The argument to value=, or value>= or value> if any of
**      bits 0x0380 are in the idx_num mask
**    * The argument to value<= or value< if either of bits 0x3000
**      are in the mask
**
*/
static int __series_best_index(
  dbsql_vtab_t *v_tab,
  dbsql_index_info_t *idx_info
){
  int i, j;              /* Loop over constraints */
  int idx_num = 0;        /* The query plan bitmask */
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
  int start_seen = 0;    /* EQ constraint seen on the START column */
#endif
  int unusable_mask = 0;  /* Mask of unusable constraints */
  int n_arg = 0;          /* Number of arguments that __series_filter() expects */
  int idx[7];           /* Constraints on start, stop, step, LIMIT, OFFSET,
                         ** and value.  idx[5] covers value=, value>=, and
                         ** value>,  idx[6] covers value<= and value< */
  const struct dbsql_index_constraint_t *constraint;

  /* This implementation assumes that the start, stop, and step columns
  ** are the last three columns in the virtual table. */
  assert( SERIES_COLUMN_STOP == SERIES_COLUMN_START+1 );
  assert( SERIES_COLUMN_STEP == SERIES_COLUMN_START+2 );

  idx[0] = idx[1] = idx[2] = idx[3] = idx[4] = idx[5] = idx[6] = -1;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    int i_col;    /* 0 for start, 1 for stop, 2 for step */
    int i_mask;   /* bitmask for those column */
    int op = constraint->op;
    if( op>=DBSQL_INDEX_CONSTRAINT_LIMIT
     && op<=DBSQL_INDEX_CONSTRAINT_OFFSET
    ){
      if( constraint->usable==0 ){
        /* do nothing */
      }else if( op==DBSQL_INDEX_CONSTRAINT_LIMIT ){
        idx[3] = i;
        idx_num |= 0x20;
      }else{
        assert( op==DBSQL_INDEX_CONSTRAINT_OFFSET );
        idx[4] = i;
        idx_num |= 0x40;
      }
      continue;
    }
    if( constraint->i_column<SERIES_COLUMN_START ){
      if( (constraint->i_column==SERIES_COLUMN_VALUE ||
           constraint->i_column==SERIES_COLUMN_ROWID)
       && constraint->usable
      ){
        switch( op ){
          case DBSQL_INDEX_CONSTRAINT_EQ:
          case DBSQL_INDEX_CONSTRAINT_IS: {
            idx_num |=  0x0080;
            idx_num &= ~0x3300;
            idx[5] = i;
            idx[6] = -1;
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
            start_seen = 1;
#endif
            break;
          }
          case DBSQL_INDEX_CONSTRAINT_GE: {
            if( idx_num & 0x0080 ) break;
            idx_num |=  0x0100;
            idx_num &= ~0x0200;
            idx[5] = i;
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
            start_seen = 1;
#endif
            break;
          }
          case DBSQL_INDEX_CONSTRAINT_GT: {
            if( idx_num & 0x0080 ) break;
            idx_num |=  0x0200;
            idx_num &= ~0x0100;
            idx[5] = i;
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
            start_seen = 1;
#endif
            break;
          }
          case DBSQL_INDEX_CONSTRAINT_LE: {
            if( idx_num & 0x0080 ) break;
            idx_num |=  0x1000;
            idx_num &= ~0x2000;
            idx[6] = i;
            break;
          }
          case DBSQL_INDEX_CONSTRAINT_LT: {
            if( idx_num & 0x0080 ) break;
            idx_num |=  0x2000;
            idx_num &= ~0x1000;
            idx[6] = i;
            break;
          }
        }
      }
      continue;
    }
    i_col = constraint->i_column - SERIES_COLUMN_START;
    assert( i_col>=0 && i_col<=2 );
    i_mask = 1 << i_col;
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
    if( i_col==0 && op==DBSQL_INDEX_CONSTRAINT_EQ ){
      start_seen = 1;
    }
#endif
    if( constraint->usable==0 ){
      unusable_mask |=  i_mask;
      continue;
    }else if( op==DBSQL_INDEX_CONSTRAINT_EQ ){
      idx_num |= i_mask;
      idx[i_col] = i;
    }
  }
  if( idx[3]==0 ){
    /* Ignore OFFSET if LIMIT is omitted */
    idx_num &= ~0x60;
    idx[4] = 0;
  }
  for(i=0; i<7; i++){
    if( (j = idx[i])>=0 ){
      idx_info->constraint_usages[j].argv_index = ++n_arg;
      idx_info->constraint_usages[j].omit =
         !DBSQL_SERIES_CONSTRAINT_VERIFY || i>=3;
    }
  }
  /* The current generate_column() implementation requires at least one
  ** argument (the START value).  Legacy versions assumed START=0 if the
  ** first argument was omitted.  Compile with -DZERO_ARGUMENT_GENERATE_SERIES
  ** to obtain the legacy behavior */
#ifndef ZERO_ARGUMENT_GENERATE_SERIES
  if( !start_seen ){
    dbsql_free(v_tab->errmsg);
    v_tab->errmsg = dbsql_mprintf(
        "first argument to \"generate_series()\" missing or unusable");
    return DBSQL_ERROR;
  }
#endif
  if( (unusable_mask & ~idx_num)!=0 ){
    /* The start, stop, and step columns are inputs.  Therefore if there
    ** are unusable constraints on any of start, stop, or step then
    ** this plan is unusable */
    return DBSQL_CONSTRAINT;
  }
  if( (idx_num & 0x03)==0x03 ){
    /* Both start= and stop= boundaries are available.  This is the 
    ** the preferred case */
    idx_info->estimated_cost = (double)(2 - ((idx_num&4)!=0));
    idx_info->estimated_rows = 1000;
    if( idx_info->n_order_by>=1 && idx_info->order_bies[0].i_column==0 ){
      if( idx_info->order_bies[0].desc ){
        idx_num |= 0x08;
      }else{
        idx_num |= 0x10;
      }
      idx_info->order_by_consumed = 1;
    }
  }else if( (idx_num & 0x21)==0x21 ){
    /* We have start= and LIMIT */
    idx_info->estimated_rows = 2500;
  }else{
    /* If either boundary is missing, we have to generate a huge span
    ** of numbers.  Make this case very expensive so that the query
    ** planner will work hard to avoid it. */
    idx_info->estimated_rows = 2147483647;
  }
  idx_info->idx_num = idx_num;
#ifdef DBSQL_INDEX_SCAN_HEX
  idx_info->idx_flags = DBSQL_INDEX_SCAN_HEX;
#endif
  return DBSQL_OK;
}

/*
** This following structure defines all the methods for the 
** generate_series virtual table.
*/
static dbsql_module_t __series_module = {
  0,                         /* i_version */
  0,                         /* create */
  __series_connect,             /* connect */
  __series_best_index,           /* best_index */
  __series_disconnect,          /* disconnect */
  0,                         /* destroy */
  __series_open,                /* x_open - open a cursor */
  __series_close,               /* x_close - close a cursor */
  __series_filter,              /* filter - configure scan constraints */
  __series_next,                /* next - advance a cursor */
  __series_eof,                 /* eof - check for end of scan */
  __series_column,              /* column - read data */
  __series_rowid,               /* rowid - read data */
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

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_series_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( dbsql_libversion_number()<3008012 && errmsg_p!=0 ){
    *errmsg_p = dbsql_mprintf(
        "generate_series() requires dbsql 3.8.12 or later");
    return DBSQL_ERROR;
  }
  rc = dbsql_create_module(db, "generate_series", &__series_module, 0);
#endif
  return rc;
}
