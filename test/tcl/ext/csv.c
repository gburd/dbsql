/*
** 2016-05-28
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
** This file contains the implementation of an dbsql virtual table for
** reading CSV files.
**
** Usage:
**
**    .load ./csv
**    CREATE VIRTUAL TABLE temp.csv USING csv(filename=FILENAME);
**    SELECT * FROM csv;
**
** The columns are named "c1", "c2", "c3", ... by default.  Or the
** application can define its own CREATE TABLE statement using the
** schema= parameter, like this:
**
**    CREATE VIRTUAL TABLE temp.csv2 USING csv(
**       filename = '../http.log',
**       schema = 'CREATE TABLE x(date,ipaddr,url,referrer,user_agent)'
**    );
**
** Instead of specifying a file, the text of the CSV can be loaded using
** the data= parameter.
**
** If the columns=N parameter is supplied, then the CSV file is assumed to have
** N columns.  If both the columns= and schema= parameters are omitted, then
** the number and names of the columns is determined by the first line of
** the CSV input.
**
** Some extra debugging features (used for testing virtual tables) are available
** if this module is compiled with -DSQLITE_TEST.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** A macro to hint to the compiler that a function should not be
** inlined.
*/
#if defined(__GNUC__)
#  define CSV_NOINLINE  __attribute__((noinline))
#elif defined(_MSC_VER) && _MSC_VER>=1310
#  define CSV_NOINLINE  __declspec(noinline)
#else
#  define CSV_NOINLINE
#endif

#ifndef DBSQLINT_H
typedef int64_t int64_t;
typedef u_int64_t u_int64_t;
#endif

/* Max size of the error message in a csv_reader_t */
#define CSV_MXERR 200

/* Size of the csv_reader_t input buffer */
#define CSV_INBUFSZ 1024

/* A context object used when read a CSV file. */
typedef struct csv_reader_t csv_reader_t;
struct csv_reader_t {
  FILE *in;              /* Read the CSV text from this input stream */
  char *z;               /* Accumulated text for a field */
  int64_t n;                 /* Number of bytes in z */
  int64_t n_alloc;            /* Space allocated for z[] */
  int64_t n_line;             /* Current line number */
  int not_first;         /* True if prior text has been seen */
  int c_term;             /* Character that terminated the most recent field */
  size_t i_in;            /* Next unread character in the input buffer */
  size_t n_in;            /* Number of characters in the input buffer */
  char *z_in;             /* The input buffer */
  char err[CSV_MXERR];  /* Error message */
};

/* Initialize a csv_reader_t object */
static void csv_reader_init(csv_reader_t *p){
  p->in = 0;
  p->z = 0;
  p->n = 0;
  p->n_alloc = 0;
  p->n_line = 0;
  p->not_first = 0;
  p->n_in = 0;
  p->z_in = 0;
  p->err[0] = 0;
}

/* Close and reset a csv_reader_t object */
static void csv_reader_reset(csv_reader_t *p){
  if( p->in ){
    fclose(p->in);
    dbsql_free(p->z_in);
  }
  dbsql_free(p->z);
  csv_reader_init(p);
}

/* Report an error on a csv_reader_t */
static void csv_errmsg(csv_reader_t *p, const char *format, ...){
  va_list ap;
  va_start(ap, format);
  dbsql_vsnprintf(CSV_MXERR, p->err, format, ap);
  va_end(ap);
}

/* Open the file associated with a csv_reader_t
** Return the number of errors.
*/
static int csv_reader_open(
  csv_reader_t *p,               /* The reader to open */
  const char *filename,      /* Read from this filename */
  const char *data           /*  ... or use this data */
){
  if( filename ){
    p->z_in = dbsql_malloc64( CSV_INBUFSZ );
    if( p->z_in==0 ){
      csv_errmsg(p, "out of memory");
      return 1;
    }
    p->in = fopen(filename, "rb");
    if( p->in==0 ){
      dbsql_free(p->z_in);
      csv_reader_reset(p);
      csv_errmsg(p, "cannot open '%s' for reading", filename);
      return 1;
    }
  }else{
    assert( p->in==0 );
    p->z_in = (char*)data;
    p->n_in = strlen(data);
  }
  return 0;
}

/* The input buffer has overflowed.  Refill the input buffer, then
** return the next character
*/
static CSV_NOINLINE int csv_getc_refill(csv_reader_t *p){
  size_t got;

  assert( p->i_in>=p->n_in );  /* Only called on an empty input buffer */
  assert( p->in!=0 );        /* Only called if reading froma file */

  got = fread(p->z_in, 1, CSV_INBUFSZ, p->in);
  if( got==0 ) return EOF;
  p->n_in = got;
  p->i_in = 1;
  return p->z_in[0];
}

/* Return the next character of input.  Return EOF at end of input. */
static int csv_getc(csv_reader_t *p){
  if( p->i_in >= p->n_in ){
    if( p->in!=0 ) return csv_getc_refill(p);
    return EOF;
  }
  return ((unsigned char*)p->z_in)[p->i_in++];
}

/* Increase the size of p->z and append character c to the end. 
** Return 0 on success and non-zero if there is an OOM error */
static CSV_NOINLINE int csv_resize_and_append(csv_reader_t *p, char c){
  char *z_new;
  int64_t n_new = p->n_alloc*2 + 100;
  z_new = dbsql_realloc64(p->z, n_new);
  if( z_new ){
    p->z = z_new;
    p->n_alloc = n_new;
    p->z[p->n++] = c;
    return 0;
  }else{
    csv_errmsg(p, "out of memory");
    return 1;
  }
}

/* Append a single character to the csv_reader_t.z[] array.
** Return 0 on success and non-zero if there is an OOM error */
static int csv_append(csv_reader_t *p, char c){
  if( p->n>=p->n_alloc-1 ) return csv_resize_and_append(p, c);
  p->z[p->n++] = c;
  return 0;
}

/* Read a single field of CSV text.  Compatible with rfc4180 and extended
** with the option of having a separator other than ",".
**
**   +  Input comes from p->in.
**   +  Store results in p->z of length p->n.  Space to hold p->z comes
**      from dbsql_malloc64().
**   +  Keep track of the line number in p->n_line.
**   +  Store the character that terminates the field in p->c_term.  Store
**      EOF on end-of-file.
**
** Return 0 at EOF or on OOM.  On EOF, the p->c_term character will have
** been set to EOF.
*/
static char *csv_read_one_field(csv_reader_t *p){
  int c;
  p->n = 0;
  c = csv_getc(p);
  if( c==EOF ){
    p->c_term = EOF;
    return 0;
  }
  if( c=='"' ){
    int pc, ppc;
    int64_t start_line = p->n_line;
    pc = ppc = 0;
    while( 1 ){
      c = csv_getc(p);
      if( c<='"' || pc=='"' ){
        if( c=='\n' ) p->n_line++;
        if( c=='"' ){
          if( pc=='"' ){
            pc = 0;
            continue;
          }
        }
        if( (c==',' && pc=='"')
         || (c=='\n' && pc=='"')
         || (c=='\n' && pc=='\r' && ppc=='"')
         || (c==EOF && pc=='"')
        ){
          do{ p->n--; }while( p->z[p->n]!='"' );
          p->c_term = (char)c;
          break;
        }
        if( pc=='"' && c!='\r' ){
          csv_errmsg(p, "line %d: unescaped %c character", p->n_line, '"');
          break;
        }
        if( c==EOF ){
          csv_errmsg(p, "line %d: unterminated %c-quoted field\n",
                     start_line, '"');
          p->c_term = (char)c;
          break;
        }
      }
      if( csv_append(p, (char)c) ) return 0;
      ppc = pc;
      pc = c;
    }
  }else{
    /* If this is the first field being parsed and it begins with the
    ** UTF-8 BOM  (0xEF BB BF) then skip the BOM */
    if( (c&0xff)==0xef && p->not_first==0 ){
      csv_append(p, (char)c);
      c = csv_getc(p);
      if( (c&0xff)==0xbb ){
        csv_append(p, (char)c);
        c = csv_getc(p);
        if( (c&0xff)==0xbf ){
          p->not_first = 1;
          p->n = 0;
          return csv_read_one_field(p);
        }
      }
    }
    while( c>',' || (c!=EOF && c!=',' && c!='\n') ){
      if( csv_append(p, (char)c) ) return 0;
      c = csv_getc(p);
    }
    if( c=='\n' ){
      p->n_line++;
      if( p->n>0 && p->z[p->n-1]=='\r' ) p->n--;
    }
    p->c_term = (char)c;
  }
  assert( p->z==0 || p->n<p->n_alloc );
  if( p->z ) p->z[p->n] = 0;
  p->not_first = 1;
  return p->z;
}


/* Forward references to the various virtual table methods implemented
** in this file. */
static int __csvtab_create(DBSQL*, void*, int, const char*const*, 
                           dbsql_vtab_t**,char**);
static int __csvtab_connect(DBSQL*, void*, int, const char*const*, 
                           dbsql_vtab_t**,char**);
static int __csvtab_best_index(dbsql_vtab_t*,dbsql_index_info_t*);
static int __csvtab_disconnect(dbsql_vtab_t*);
static int __csvtab_open(dbsql_vtab_t*, dbsql_vtab_cursor_t**);
static int __csvtab_close(dbsql_vtab_cursor_t*);
static int __csvtab_filter(dbsql_vtab_cursor_t*, int idx_num, const char *idx_str,
                          int argc, dbsql_value_t **argv);
static int __csvtab_next(dbsql_vtab_cursor_t*);
static int __csvtab_eof(dbsql_vtab_cursor_t*);
static int __csvtab_column(dbsql_vtab_cursor_t*,dbsql_func_t*,int);
static int __csvtab_rowid(dbsql_vtab_cursor_t*,int64_t*);

/* An instance of the CSV virtual table */
typedef struct csv_table_t {
  dbsql_vtab_t base;              /* Base class.  Must be first */
  char *filename;                /* Name of the CSV file */
  char *data;                    /* Raw CSV data in lieu of filename */
  long i_start;                    /* Offset to start of data in filename */
  int n_col;                       /* Number of columns in the CSV file */
  unsigned int tst_flags;          /* Bit values used for testing */
} csv_table_t;

/* Allowed values for tst_flags */
#define CSVTEST_FIDX  0x0001      /* Pretend that constrained search cost less*/

/* A cursor for the CSV virtual table */
typedef struct csv_cursor_t {
  dbsql_vtab_cursor_t base;       /* Base class.  Must be first */
  csv_reader_t rdr;                  /* The csv_reader_t object */
  char **vals;                   /* Value of the current row */
  int64_t *lens;                      /* Length of each entry */
  int64_t i_rowid;           /* The current rowid.  Negative for EOF */
} csv_cursor_t;

/* Transfer error message text from a reader into a csv_table_t */
static void csv_xfer_error(csv_table_t *tab, csv_reader_t *rdr){
  dbsql_free(tab->base.errmsg);
  tab->base.errmsg = dbsql_mprintf("%s", rdr->err);
}

/*
** This method is the destructor fo a csv_table_t object.
*/
static int __csvtab_disconnect(dbsql_vtab_t *vtab){
  csv_table_t *p = (csv_table_t*)vtab;
  dbsql_free(p->filename);
  dbsql_free(p->data);
  dbsql_free(p);
  return DBSQL_OK;
}

/* Skip leading whitespace.  Return a pointer to the first non-whitespace
** character, or to the zero terminator if the string has only whitespace */
static const char *csv_skip_whitespace(const char *z){
  while( isspace((unsigned char)z[0]) ) z++;
  return z;
}

/* Remove trailing whitespace from the end of string z[] */
static void csv_trim_whitespace(char *z){
  size_t n = strlen(z);
  while( n>0 && isspace((unsigned char)z[n]) ) n--;
  z[n] = 0;
}

/* Dequote the string */
static void csv_dequote(char *z){
  int j;
  char c_quote = z[0];
  size_t i, n;

  if( c_quote!='\'' && c_quote!='"' ) return;
  n = strlen(z);
  if( n<2 || z[n-1]!=z[0] ) return;
  for(i=1, j=0; i<n-1; i++){
    if( z[i]==c_quote && z[i+1]==c_quote ) i++;
    z[j++] = z[i];
  }
  z[j] = 0;
}

/* Check to see if the string is of the form:  "TAG = VALUE" with optional
** whitespace before and around tokens.  If it is, return a pointer to the
** first character of VALUE.  If it is not, return NULL.
*/
static const char *csv_parameter(const char *tag, int n_tag, const char *z){
  z = csv_skip_whitespace(z);
  if( strncmp(tag, z, n_tag)!=0 ) return 0;
  z = csv_skip_whitespace(z+n_tag);
  if( z[0]!='=' ) return 0;
  return csv_skip_whitespace(z+1);
}

/* Decode a parameter that requires a dequoted string.
**
** Return 1 if the parameter is seen, or 0 if not.  1 is returned
** even if there is an error.  If an error occurs, then an error message
** is left in p->err.  If there are no errors, p->err[0]==0.
*/
static int csv_string_parameter(
  csv_reader_t *p,            /* Leave the error message here, if there is one */
  const char *param,      /* Parameter we are checking for */
  const char *arg,        /* Raw text of the virtual table argment */
  char **val             /* Write the dequoted string value here */
){
  const char *value;
  value = csv_parameter(param,(int)strlen(param),arg);
  if( value==0 ) return 0;
  p->err[0] = 0;
  if( *val ){
    csv_errmsg(p, "more than one '%s' parameter", param);
    return 1;
  }
  *val = dbsql_mprintf("%s", value);
  if( *val==0 ){
    csv_errmsg(p, "out of memory");
    return 1;
  }
  csv_trim_whitespace(*val);
  csv_dequote(*val);
  return 1;
}


/* Return 0 if the argument is false and 1 if it is true.  Return -1 if
** we cannot really tell.
*/
static int csv_boolean(const char *z){
  if( dbsql_stricmp("yes",z)==0
   || dbsql_stricmp("on",z)==0
   || dbsql_stricmp("true",z)==0
   || (z[0]=='1' && z[1]==0)
  ){
    return 1;
  }
  if( dbsql_stricmp("no",z)==0
   || dbsql_stricmp("off",z)==0
   || dbsql_stricmp("false",z)==0
   || (z[0]=='0' && z[1]==0)
  ){
    return 0;
  }
  return -1;
}

/* Check to see if the string is of the form:  "TAG = BOOLEAN" or just "TAG".
** If it is, set *value to be the value of the boolean ("true" if there is
** not "= BOOLEAN" component) and return non-zero.  If the input string
** does not begin with TAG, return zero.
*/
static int csv_boolean_parameter(
  const char *tag,       /* Tag we are looking for */
  int n_tag,               /* Size of the tag in bytes */
  const char *z,          /* Input parameter */
  int *value             /* Write boolean value here */
){
  int b;
  z = csv_skip_whitespace(z);
  if( strncmp(tag, z, n_tag)!=0 ) return 0;
  z = csv_skip_whitespace(z + n_tag);
  if( z[0]==0 ){
    *value = 1;
    return 1;
  }
  if( z[0]!='=' ) return 0;
  z = csv_skip_whitespace(z+1);
  b = csv_boolean(z);
  if( b>=0 ){
    *value = b;
    return 1;
  }
  return 0;
}

/*
** Parameters:
**    filename=FILENAME          Name of file containing CSV content
**    data=TEXT                  Direct CSV content.
**    schema=SCHEMA              Alternative CSV schema.
**    header=YES|NO              First row of CSV defines the names of
**                               columns if "yes".  Default "no".
**    columns=N                  Assume the CSV file contains N columns.
**
** Only available if compiled with DBSQL_TEST:
**    
**    testflags=N                Bitmask of test flags.  Optional
**
** If schema= is omitted, then the columns are named "c0", "c1", "c2",
** and so forth.  If columns=N is omitted, then the file is opened and
** the number of columns in the first row is counted to determine the
** column count.  If header=YES, then the first row is skipped.
*/
static int __csvtab_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  csv_table_t *p_new = 0;        /* The csv_table_t object to construct */
  int header = -1;          /* header= flags.  -1 means not seen yet */
  int rc = DBSQL_OK;        /* Result code from this routine */
  u_int64_t i, j;                  /* Loop counters */
#ifdef DBSQL_TEST
  int tst_flags = 0;          /* Value for testflags=N parameter */
#endif
  int b;                     /* Value of a boolean parameter */
  int n_col = -99;            /* Value of the columns= parameter */
  csv_reader_t s_rdr;            /* A CSV file reader used to store an error
                             ** message and/or to count the number of columns */
  static const char *params[] = {
     "filename", "data", "schema", 
  };
  char *p_values[3];         /* Parameter values */
# define CSV_FILENAME (p_values[0])
# define CSV_DATA     (p_values[1])
# define CSV_SCHEMA   (p_values[2])

  assert( sizeof(p_values)==sizeof(params) );
  memset(&s_rdr, 0, sizeof(s_rdr));
  memset(p_values, 0, sizeof(p_values));
  for(i=3; i<(u_int64_t)argc; i++){
    const char *z = argv[i];
    const char *value;
    for(j=0; j<sizeof(params)/sizeof(params[0]); j++){
      if( csv_string_parameter(&s_rdr, params[j], z, &p_values[j]) ) break;
    }
    if( j<sizeof(params)/sizeof(params[0]) ){
      if( s_rdr.err[0] ) goto csvtab_connect_error;
    }else
    if( csv_boolean_parameter("header",6,z,&b) ){
      if( header>=0 ){
        csv_errmsg(&s_rdr, "more than one 'header' parameter");
        goto csvtab_connect_error;
      }
      header = b;
    }else
#ifdef DBSQL_TEST
    if( (value = csv_parameter("testflags",9,z))!=0 ){
      tst_flags = (unsigned int)atoi(value);
    }else
#endif
    if( (value = csv_parameter("columns",7,z))!=0 ){
      if( n_col>0 ){
        csv_errmsg(&s_rdr, "more than one 'columns' parameter");
        goto csvtab_connect_error;
      }
      n_col = atoi(value);
      if( n_col<=0 ){
        csv_errmsg(&s_rdr, "column= value must be positive");
        goto csvtab_connect_error;
      }
    }else
    {
      csv_errmsg(&s_rdr, "bad parameter: '%s'", z);
      goto csvtab_connect_error;
    }
  }
  if( (CSV_FILENAME==0)==(CSV_DATA==0) ){
    csv_errmsg(&s_rdr, "must specify either filename= or data= but not both");
    goto csvtab_connect_error;
  }

  if( (n_col<=0 || header==1)
   && csv_reader_open(&s_rdr, CSV_FILENAME, CSV_DATA)
  ){
    goto csvtab_connect_error;
  }
  p_new = dbsql_malloc64( sizeof(*p_new) );
  *vtab = (dbsql_vtab_t*)p_new;
  if( p_new==0 ) goto csvtab_connect_oom;
  memset(p_new, 0, sizeof(*p_new));
  if( CSV_SCHEMA==0 ){
    dbsql_str_t *str = dbsql_str_new(0);
    char *sep = "";
    int i_col = 0;
    dbsql_str_appendf(str, "CREATE TABLE x(");
    if( n_col<0 && header<1 ){
      n_col = 0;
      do{
        csv_read_one_field(&s_rdr);
        n_col++;
      }while( s_rdr.c_term==',' );
    }
    if( n_col>0 && header<1 ){
      for(i_col=0; i_col<n_col; i_col++){
        dbsql_str_appendf(str, "%sc%d TEXT", sep, i_col);
        sep = ",";
      }
    }else{
      do{
        char *z = csv_read_one_field(&s_rdr);
        if( (n_col>0 && i_col<n_col) || (n_col<0 && header) ){
          dbsql_str_appendf(str,"%s\"%w\" TEXT", sep, z);
          sep = ",";
          i_col++;
        }
      }while( s_rdr.c_term==',' );
      if( n_col<0 ){
        n_col = i_col;
      }else{
        while( i_col<n_col ){
          dbsql_str_appendf(str,"%sc%d TEXT", sep, ++i_col);
          sep = ",";
        }
      }
    }
    p_new->n_col = n_col;
    dbsql_str_appendf(str, ")");
    CSV_SCHEMA = dbsql_str_finish(str);
    if( CSV_SCHEMA==0 ) goto csvtab_connect_oom;
  }else if( n_col<0 ){
    do{
      csv_read_one_field(&s_rdr);
      p_new->n_col++;
    }while( s_rdr.c_term==',' );
  }else{
    p_new->n_col = n_col;
  }
  p_new->filename = CSV_FILENAME;  CSV_FILENAME = 0;
  p_new->data = CSV_DATA;          CSV_DATA = 0;
#ifdef DBSQL_TEST
  p_new->tst_flags = tst_flags;
#endif
  if( header!=1 ){
    p_new->i_start = 0;
  }else if( p_new->data ){
    p_new->i_start = (int)s_rdr.i_in;
  }else{
    p_new->i_start = (int)(ftell(s_rdr.in) - s_rdr.n_in + s_rdr.i_in);
  }
  csv_reader_reset(&s_rdr);
  rc = dbsql_declare_vtab(db, CSV_SCHEMA);
  if( rc ){
    csv_errmsg(&s_rdr, "bad schema: '%s' - %s", CSV_SCHEMA, dbsql_errmsg(db));
    goto csvtab_connect_error;
  }
  for(i=0; i<sizeof(p_values)/sizeof(p_values[0]); i++){
    dbsql_free(p_values[i]);
  }
  /* Rationale for DIRECTONLY:
  ** An attacker who controls a database schema could use this vtab
  ** to exfiltrate sensitive data from other files in the filesystem.
  ** And, recommended practice is to put all CSV virtual tables in the
  ** TEMP namespace, so they should still be usable from within TEMP
  ** views, so there shouldn't be a serious loss of functionality by
  ** prohibiting the use of this vtab from persistent triggers and views.
  */
  dbsql_vtab_config(db, DBSQL_VTAB_DIRECTONLY);
  return DBSQL_OK;

csvtab_connect_oom:
  rc = DBSQL_NOMEM;
  csv_errmsg(&s_rdr, "out of memory");

csvtab_connect_error:
  if( p_new ) __csvtab_disconnect(&p_new->base);
  for(i=0; i<sizeof(p_values)/sizeof(p_values[0]); i++){
    dbsql_free(p_values[i]);
  }
  if( s_rdr.err[0] ){
    dbsql_free(*err_p);
    *err_p = dbsql_mprintf("%s", s_rdr.err);
  }
  csv_reader_reset(&s_rdr);
  if( rc==DBSQL_OK ) rc = DBSQL_ERROR;
  return rc;
}

/*
** Reset the current row content held by a csv_cursor_t.
*/
static void __csvtab_cursor_row_reset(csv_cursor_t *cur){
  csv_table_t *tab = (csv_table_t*)cur->base.vtab;
  int i;
  for(i=0; i<tab->n_col; i++){
    dbsql_free(cur->vals[i]);
    cur->vals[i] = 0;
    cur->lens[i] = 0;
  }
}

/*
** The connect and create methods do the same thing, but they must be
** different so that the virtual table is not an eponymous virtual table.
*/
static int __csvtab_create(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
 return __csvtab_connect(db, aux, argc, argv, vtab, err_p);
}

/*
** Destructor for a csv_cursor_t.
*/
static int __csvtab_close(dbsql_vtab_cursor_t *cur){
  csv_cursor_t *p_cur = (csv_cursor_t*)cur;
  __csvtab_cursor_row_reset(p_cur);
  csv_reader_reset(&p_cur->rdr);
  dbsql_free(cur);
  return DBSQL_OK;
}

/*
** Constructor for a new csv_table_t cursor object.
*/
static int __csvtab_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  csv_table_t *tab = (csv_table_t*)p;
  csv_cursor_t *cur;
  size_t n_byte;
  n_byte = sizeof(*cur) + (sizeof(char*)+sizeof(int64_t))*tab->n_col;
  cur = dbsql_malloc64( n_byte );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, n_byte);
  cur->vals = (char**)&cur[1];
  cur->lens = (int64_t*)&cur->vals[tab->n_col];
  *cursor = &cur->base;
  if( csv_reader_open(&cur->rdr, tab->filename, tab->data) ){
    csv_xfer_error(tab, &cur->rdr);
    return DBSQL_ERROR;
  }
  return DBSQL_OK;
}


/*
** Advance a csv_cursor_t to its next row of input.
** Set the EOF marker if we reach the end of input.
*/
static int __csvtab_next(dbsql_vtab_cursor_t *cur){
  csv_cursor_t *p_cur = (csv_cursor_t*)cur;
  csv_table_t *tab = (csv_table_t*)cur->vtab;
  int i = 0;
  char *z;
  do{
    z = csv_read_one_field(&p_cur->rdr);
    if( z==0 ){
      break;
    }
    if( i<tab->n_col ){
      if( p_cur->lens[i] < p_cur->rdr.n+1 ){
        char *z_new = dbsql_realloc64(p_cur->vals[i], p_cur->rdr.n+1);
        if( z_new==0 ){
          csv_errmsg(&p_cur->rdr, "out of memory");
          csv_xfer_error(tab, &p_cur->rdr);
          break;
        }
        p_cur->vals[i] = z_new;
        p_cur->lens[i] = p_cur->rdr.n+1;
      }
      memcpy(p_cur->vals[i], z, p_cur->rdr.n+1);
      i++;
    }
  }while( p_cur->rdr.c_term==',' );
  if( z==0 && i==0 ){
    p_cur->i_rowid = -1;
  }else{
    p_cur->i_rowid++;
    while( i<tab->n_col ){
      dbsql_free(p_cur->vals[i]);
      p_cur->vals[i] = 0;
      p_cur->lens[i] = 0;
      i++;
    }
  }
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the csv_cursor_t
** is currently pointing.
*/
static int __csvtab_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  csv_cursor_t *p_cur = (csv_cursor_t*)cur;
  csv_table_t *tab = (csv_table_t*)cur->vtab;
  if( i>=0 && i<tab->n_col && p_cur->vals[i]!=0 ){
    dbsql_result_text(ctx, p_cur->vals[i], -1, DBSQL_TRANSIENT);
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.
*/
static int __csvtab_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  csv_cursor_t *p_cur = (csv_cursor_t*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __csvtab_eof(dbsql_vtab_cursor_t *cur){
  csv_cursor_t *p_cur = (csv_cursor_t*)cur;
  return p_cur->i_rowid<0;
}

/*
** Only a full table scan is supported.  So filter simply rewinds to
** the beginning.
*/
static int __csvtab_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  csv_cursor_t *cur = (csv_cursor_t*)vtab_cursor;
  csv_table_t *tab = (csv_table_t*)vtab_cursor->vtab;
  cur->i_rowid = 0;

  /* Ensure the field buffer is always allocated. Otherwise, if the
  ** first field is zero bytes in size, this may be mistaken for an OOM
  ** error in __csvtab_next(). */
  if( csv_append(&cur->rdr, 0) ) return DBSQL_NOMEM;

  if( cur->rdr.in==0 ){
    assert( cur->rdr.z_in==tab->data );
    assert( tab->i_start>=0 );
    assert( (size_t)tab->i_start<=cur->rdr.n_in );
    cur->rdr.i_in = tab->i_start;
  }else{
    fseek(cur->rdr.in, tab->i_start, SEEK_SET);
    cur->rdr.i_in = 0;
    cur->rdr.n_in = 0;
  }
  return __csvtab_next(vtab_cursor);
}

/*
** Only a forward full table scan is supported.  best_index is mostly
** a no-op.  If CSVTEST_FIDX is set, then the presence of equality
** constraints lowers the estimated cost, which is fiction, but is useful
** for testing certain kinds of virtual table behavior.
*/
static int __csvtab_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  idx_info->estimated_cost = 1000000;
#ifdef DBSQL_TEST
  if( (((csv_table_t*)tab)->tst_flags & CSVTEST_FIDX)!=0 ){
    /* The usual (and sensible) case is to always do a full table scan.
    ** The code in this branch only runs when testflags=1.  This code
    ** generates an artifical and unrealistic plan which is useful
    ** for testing virtual table logic but is not helpful to real applications.
    **
    ** Any ==, LIKE, or GLOB constraint is marked as usable by the virtual
    ** table (even though it is not) and the cost of running the virtual table
    ** is reduced from 1 million to just 10.  The constraints are *not* marked
    ** as omittable, however, so the query planner should still generate a
    ** plan that gives a correct answer, even if they plan is not optimal.
    */
    int i;
    int n_const = 0;
    for(i=0; i<idx_info->n_constraint; i++){
      unsigned char op;
      if( idx_info->constraints[i].usable==0 ) continue;
      op = idx_info->constraints[i].op;
      if( op==DBSQL_INDEX_CONSTRAINT_EQ 
       || op==DBSQL_INDEX_CONSTRAINT_LIKE
       || op==DBSQL_INDEX_CONSTRAINT_GLOB
      ){
        idx_info->estimated_cost = 10;
        idx_info->constraint_usages[n_const].argv_index = n_const+1;
        n_const++;
      }
    }
  }
#endif
  return DBSQL_OK;
}


static dbsql_module_t __csv_module = {
  0,                       /* i_version */
  __csvtab_create,            /* create */
  __csvtab_connect,           /* connect */
  __csvtab_best_index,         /* best_index */
  __csvtab_disconnect,        /* disconnect */
  __csvtab_disconnect,        /* destroy */
  __csvtab_open,              /* x_open - open a cursor */
  __csvtab_close,             /* x_close - close a cursor */
  __csvtab_filter,            /* filter - configure scan constraints */
  __csvtab_next,              /* next - advance a cursor */
  __csvtab_eof,               /* eof - check for end of scan */
  __csvtab_column,            /* column - read data */
  __csvtab_rowid,             /* rowid - read data */
  0,                       /* update */
  0,                       /* begin */
  0,                       /* x_sync */
  0,                       /* commit */
  0,                       /* rollback */
  0,                       /* find_method */
  0,                       /* rename */
  0,                       /* savepoint */
  0,                       /* release */
  0,                       /* rollback_to */
  0,                       /* shadow_name */
  0                        /* integrity */
};

#ifdef DBSQL_TEST
/*
** For virtual table testing, make a version of the CSV virtual table
** available that has an update function.  But the update always returns
** DBSQL_READONLY since the CSV file is not really writable.
*/
static int __csvtab_update(dbsql_vtab_t *p,int n,dbsql_value_t**v,int64_t*x){
  return DBSQL_READONLY;
}
static dbsql_module_t __csv_module_faux_write = {
  0,                       /* i_version */
  __csvtab_create,            /* create */
  __csvtab_connect,           /* connect */
  __csvtab_best_index,         /* best_index */
  __csvtab_disconnect,        /* disconnect */
  __csvtab_disconnect,        /* destroy */
  __csvtab_open,              /* x_open - open a cursor */
  __csvtab_close,             /* x_close - close a cursor */
  __csvtab_filter,            /* filter - configure scan constraints */
  __csvtab_next,              /* next - advance a cursor */
  __csvtab_eof,               /* eof - check for end of scan */
  __csvtab_column,            /* column - read data */
  __csvtab_rowid,             /* rowid - read data */
  __csvtab_update,            /* update */
  0,                       /* begin */
  0,                       /* x_sync */
  0,                       /* commit */
  0,                       /* rollback */
  0,                       /* find_method */
  0,                       /* rename */
  0,                       /* savepoint */
  0,                       /* release */
  0,                       /* rollback_to */
  0,                       /* shadow_name */
  0                        /* integrity */
};
#endif /* DBSQL_TEST */

#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) */


#ifdef _WIN32
__declspec(dllexport)
#endif
/* 
** This routine is called when the extension is loaded.  The new
** CSV virtual table module is registered with the calling database
** connection.
*/
int dbsql_csv_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  int rc;
  DBSQL_EXTENSION_INIT2(api);
  rc = dbsql_create_module(db, "csv", &__csv_module, 0);
#ifdef DBSQL_TEST
  if( rc==DBSQL_OK ){
    rc = dbsql_create_module(db, "csv_wr", &__csv_module_faux_write, 0);
  }
#endif
  return rc;
#else
  return DBSQL_OK;
#endif
}
