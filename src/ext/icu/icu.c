/*
** 2007 May 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** $Id: icu.c,v 1.7 2007/12/13 21:54:11 drh Exp $
**
** This file implements an integration between the ICU library 
** ("International Components for Unicode", an open-source library 
** for handling unicode data) and dbsql. The integration uses 
** ICU to provide the following to dbsql:
**
**   * An implementation of the SQL regexp() function (and hence REGEXP
**     operator) using the ICU uregex_XX() APIs.
**
**   * Implementations of the SQL scalar upper() and lower() functions
**     for case mapping.
**
**   * Integration of ICU and dbsql collation sequences.
**
**   * An implementation of the LIKE operator that uses ICU to 
**     provide case-independent matching.
*/

#if !defined(DBSQL_CORE)                  \
 || defined(DBSQL_ENABLE_ICU)             \
 || defined(DBSQL_ENABLE_ICU_COLLATIONS)

/* Include ICU headers */
#include <unicode/utypes.h>
#include <unicode/uregex.h>
#include <unicode/ustring.h>
#include <unicode/ucol.h>

#include <assert.h>

#ifndef DBSQL_CORE
  #include "dbsql_ext.h"
  DBSQL_EXTENSION_INIT1
#else
  #include "dbsql.h"
#endif

/*
** This function is called when an ICU function called from within
** the implementation of an SQL scalar function returns an error.
**
** The scalar function context passed as the first argument is 
** loaded with an error message based on the following two args.
*/
static void __icu_function_error(
  dbsql_func_t *ctx,       /* dbsql scalar function context */
  const char *name,           /* Name of ICU function that failed */
  UErrorCode e                 /* Error code returned by ICU function */
){
  char buf[128];
  dbsql_snprintf(128, buf, "ICU error: %s(): %s", name, u_errorName(e));
  buf[127] = '\0';
  dbsql_result_error(ctx, buf, -1);
}

#if !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_ICU)

/*
** Maximum length (in bytes) of the pattern in a LIKE or GLOB
** operator.
*/
#ifndef DBSQL_MAX_LIKE_PATTERN_LENGTH
# define DBSQL_MAX_LIKE_PATTERN_LENGTH 50000
#endif

/*
** Version of dbsql_free() that is always a function, never a macro.
*/
static void __x_free(void *p){
  dbsql_free(p);
}

/*
** This lookup table is used to help decode the first byte of
** a multi-byte UTF8 character. It is copied here from dbsql source
** code file utf8.c.
*/
static const unsigned char __icu_utf8_trans1[] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00,
};

#define DBSQL_ICU_READ_UTF8(in, c)                       \
  c = *(in++);                                            \
  if( c>=0xc0 ){                                           \
    c = __icu_utf8_trans1[c-0xc0];                             \
    while( (*in & 0xc0)==0x80 ){                          \
      c = (c<<6) + (0x3f & *(in++));                      \
    }                                                      \
  }

#define DBSQL_ICU_SKIP_UTF8(in)                          \
  assert( *in );                                          \
  if( *(in++)>=0xc0 ){                                    \
    while( (*in & 0xc0)==0x80 ){in++;}                   \
  }


/*
** Compare two UTF-8 strings for equality where the first string is
** a "LIKE" expression. Return true (1) if they are the same and 
** false (0) if they are different.
*/
static int __icu_like_compare(
  const uint8_t *pattern,   /* LIKE pattern */
  const uint8_t *string,    /* The UTF-8 string to compare against */
  const UChar32 u_esc         /* The escape character */
){
  static const uint32_t MATCH_ONE = (uint32_t)'_';
  static const uint32_t MATCH_ALL = (uint32_t)'%';

  int prev_escape = 0;     /* True if the previous character was u_esc */

  while( 1 ){

    /* Read (and consume) the next character from the input pattern. */
    uint32_t u_pattern;
    DBSQL_ICU_READ_UTF8(pattern, u_pattern);
    if( u_pattern==0 ) break;

    /* There are now 4 possibilities:
    **
    **     1. u_pattern is an unescaped match-all character "%",
    **     2. u_pattern is an unescaped match-one character "_",
    **     3. u_pattern is an unescaped escape character, or
    **     4. u_pattern is to be handled as an ordinary character
    */
    if( u_pattern==MATCH_ALL && !prev_escape && u_pattern!=(uint32_t)u_esc ){
      /* Case 1. */
      uint8_t c;

      /* Skip any MATCH_ALL or MATCH_ONE characters that follow a
      ** MATCH_ALL. For each MATCH_ONE, skip one character in the 
      ** test string.
      */
      while( (c=*pattern) == MATCH_ALL || c == MATCH_ONE ){
        if( c==MATCH_ONE ){
          if( *string==0 ) return 0;
          DBSQL_ICU_SKIP_UTF8(string);
        }
        pattern++;
      }

      if( *pattern==0 ) return 1;

      while( *string ){
        if( __icu_like_compare(pattern, string, u_esc) ){
          return 1;
        }
        DBSQL_ICU_SKIP_UTF8(string);
      }
      return 0;

    }else if( u_pattern==MATCH_ONE && !prev_escape && u_pattern!=(uint32_t)u_esc ){
      /* Case 2. */
      if( *string==0 ) return 0;
      DBSQL_ICU_SKIP_UTF8(string);

    }else if( u_pattern==(uint32_t)u_esc && !prev_escape ){
      /* Case 3. */
      prev_escape = 1;

    }else{
      /* Case 4. */
      uint32_t u_string;
      DBSQL_ICU_READ_UTF8(string, u_string);
      u_string = (uint32_t)u_foldCase((UChar32)u_string, U_FOLD_CASE_DEFAULT);
      u_pattern = (uint32_t)u_foldCase((UChar32)u_pattern, U_FOLD_CASE_DEFAULT);
      if( u_string!=u_pattern ){
        return 0;
      }
      prev_escape = 0;
    }
  }

  return *string==0;
}

/*
** Implementation of the like() SQL function.  This function implements
** the build-in LIKE operator.  The first argument to the function is the
** pattern and the second argument is the string.  So, the SQL statements:
**
**       A LIKE B
**
** is implemented as like(B, A). If there is an escape character E, 
**
**       A LIKE B ESCAPE E
**
** is mapped to like(B, A, E).
*/
static void __icu_like_func(
  dbsql_func_t *context, 
  int argc, 
  dbsql_value_t **argv
){
  const unsigned char *a = dbsql_value_text(argv[0]);
  const unsigned char *b = dbsql_value_text(argv[1]);
  UChar32 u_esc = 0;

  /* Limit the length of the LIKE or GLOB pattern to avoid problems
  ** of deep recursion and N*N behavior in __pattern_compare().
  */
  if( dbsql_value_bytes(argv[0])>DBSQL_MAX_LIKE_PATTERN_LENGTH ){
    dbsql_result_error(context, "LIKE or GLOB pattern too complex", -1);
    return;
  }


  if( argc==3 ){
    /* The escape character string must consist of a single UTF-8 character.
    ** Otherwise, return an error.
    */
    int n_e= dbsql_value_bytes(argv[2]);
    const unsigned char *e = dbsql_value_text(argv[2]);
    int i = 0;
    if( e==0 ) return;
    U8_NEXT(e, i, n_e, u_esc);
    if( i!=n_e){
      dbsql_result_error(context, 
          "ESCAPE expression must be a single character", -1);
      return;
    }
  }

  if( a && b ){
    dbsql_result_int(context, __icu_like_compare(a, b, u_esc));
  }
}

/*
** Function to delete compiled regexp objects. Registered as
** a destructor function with dbsql_set_auxdata().
*/
static void __icu_regexp_delete(void *p){
  URegularExpression *expr = (URegularExpression *)p;
  uregex_close(expr);
}

/*
** Implementation of dbsql REGEXP operator. This scalar function takes
** two arguments. The first is a regular expression pattern to compile
** the second is a string to match against that pattern. If either 
** argument is an SQL NULL, then NULL Is returned. Otherwise, the result
** is 1 if the string matches the pattern, or 0 otherwise.
**
** dbsql maps the regexp() function to the regexp() operator such
** that the following two are equivalent:
**
**     string REGEXP pattern
**     regexp(pattern, string)
**
** Uses the following ICU regexp APIs:
**
**     uregex_open()
**     uregex_matches()
**     uregex_close()
*/
static void __icu_regexp_func(dbsql_func_t *p, int n_arg, dbsql_value_t **args){
  UErrorCode status = U_ZERO_ERROR;
  URegularExpression *expr;
  UBool res;
  const UChar *string = dbsql_value_text16(args[1]);

  (void)n_arg;  /* Unused parameter */

  /* If the left hand side of the regexp operator is NULL, 
  ** then the result is also NULL. 
  */
  if( !string ){
    return;
  }

  expr = dbsql_get_auxdata(p, 0);
  if( !expr ){
    const UChar *pattern = dbsql_value_text16(args[0]);
    if( !pattern ){
      return;
    }
    expr = uregex_open(pattern, -1, 0, 0, &status);

    if( U_SUCCESS(status) ){
      dbsql_set_auxdata(p, 0, expr, __icu_regexp_delete);
      expr = dbsql_get_auxdata(p, 0);
    }
    if( !expr ){
      __icu_function_error(p, "uregex_open", status);
      return;
    }
  }

  /* Configure the text that the regular expression operates on. */
  uregex_setText(expr, string, -1, &status);
  if( !U_SUCCESS(status) ){
    __icu_function_error(p, "uregex_setText", status);
    return;
  }

  /* Attempt the match */
  res = uregex_matches(expr, 0, &status);
  if( !U_SUCCESS(status) ){
    __icu_function_error(p, "uregex_matches", status);
    return;
  }

  /* Set the text that the regular expression operates on to a NULL
  ** pointer. This is not really necessary, but it is tidier than 
  ** leaving the regular expression object configured with an invalid
  ** pointer after this function returns.
  */
  uregex_setText(expr, 0, 0, &status);

  /* Return 1 or 0. */
  dbsql_result_int(p, res ? 1 : 0);
}

/*
** Implementations of scalar functions for case mapping - upper() and 
** lower(). Function upper() converts its input to upper-case (ABC).
** Function lower() converts to lower-case (abc).
**
** ICU provides two types of case mapping, "general" case mapping and
** "language specific". Refer to ICU documentation for the differences
** between the two.
**
** To utilise "general" case mapping, the upper() or lower() scalar 
** functions are invoked with one argument:
**
**     upper('ABC') -> 'abc'
**     lower('abc') -> 'ABC'
**
** To access ICU "language specific" case mapping, upper() or lower()
** should be invoked with two arguments. The second argument is the name
** of the locale to use. Passing an empty string ("") or SQL NULL value
** as the second argument is the same as invoking the 1 argument version
** of upper() or lower().
**
**     lower('I', 'en_us') -> 'i'
**     lower('I', 'tr_tr') -> '\u131' (small dotless i)
**
** http://www.icu-project.org/userguide/posix.html#case_mappings
*/
static void __icu_case_func16(dbsql_func_t *p, int n_arg, dbsql_value_t **args){
  const UChar *input;            /* Pointer to input string */
  UChar *output = 0;             /* Pointer to output buffer */
  int n_input;                     /* Size of utf-16 input string in bytes */
  int64_t n_out;             /* Size of output buffer in bytes */
  int cnt;
  int to_upper;                   /* True for toupper(), false for tolower() */
  UErrorCode status;
  const char *locale = 0;

  assert(n_arg==1 || n_arg==2);
  to_upper = (dbsql_user_data(p)!=0);
  if( n_arg==2 ){
    locale = (const char *)dbsql_value_text(args[1]);
  }

  input = dbsql_value_text16(args[0]);
  if( !input ){
    return;
  }
  n_out = n_input = dbsql_value_bytes16(args[0]);
  if( n_out==0 ){
    dbsql_result_text16(p, "", 0, DBSQL_STATIC);
    return;
  }

  for(cnt=0; cnt<2; cnt++){
    UChar *z_new = dbsql_realloc64(output, n_out);
    if( z_new==0 ){
      dbsql_free(output);
      dbsql_result_error_nomem(p);
      return;
    }
    output = z_new;
    status = U_ZERO_ERROR;
    if( to_upper ){
      n_out = 2LL*u_strToUpper(output,n_out/2,input,n_input/2,locale,&status);
    }else{
      n_out = 2LL*u_strToLower(output,n_out/2,input,n_input/2,locale,&status);
    }

    if( U_SUCCESS(status) ){
      dbsql_result_text16(p, output, n_out, __x_free);
    }else if( status==U_BUFFER_OVERFLOW_ERROR ){
      assert( cnt==0 );
      continue;
    }else{
      __icu_function_error(p, to_upper ? "u_strToUpper" : "u_strToLower", status);
    }
    return;
  }
  assert( 0 );     /* Unreachable */
}

#endif /* !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_ICU) */

/*
** Collation sequence destructor function. The ctx argument points to
** a UCollator structure previously allocated using ucol_open().
*/
static void __icu_collation_del(void *ctx){
  UCollator *p = (UCollator *)ctx;
  ucol_close(p);
}

/*
** Collation sequence comparison function. The ctx argument points to
** a UCollator structure previously allocated using ucol_open().
*/
static int __icu_collation_coll(
  void *ctx,
  int n_left,
  const void *left,
  int n_right,
  const void *right
){
  UCollationResult res;
  UCollator *p = (UCollator *)ctx;
  res = ucol_strcoll(p, (UChar *)left, n_left/2, (UChar *)right, n_right/2);
  switch( res ){
    case UCOL_LESS:    return -1;
    case UCOL_GREATER: return +1;
    case UCOL_EQUAL:   return 0;
  }
  assert(!"Unexpected return value from ucol_strcoll()");
  return 0;
}

/*
** Implementation of the scalar function icu_load_collation().
**
** This scalar function is used to add ICU collation based collation 
** types to an dbsql database connection. It is intended to be called
** as follows:
**
**     SELECT icu_load_collation(<locale>, <collation-name>);
**
** Where <locale> is a string containing an ICU locale identifier (i.e.
** "en_AU", "tr_TR" etc.) and <collation-name> is the name of the
** collation sequence to create.
*/
static void __icu_load_collation(
  dbsql_func_t *p, 
  int n_arg, 
  dbsql_value_t **args
){
  DBSQL *db = (DBSQL *)dbsql_user_data(p);
  UErrorCode status = U_ZERO_ERROR;
  const char *locale;      /* Locale identifier - (eg. "jp_JP") */
  const char *name;        /* SQL Collation sequence name (eg. "japanese") */
  UCollator *u_collator;    /* ICU library collation object */
  int rc;                   /* Return code from dbsql_create_collation_x() */

  assert(n_arg==2 || n_arg==3);
  (void)n_arg; /* Unused parameter */
  locale = (const char *)dbsql_value_text(args[0]);
  name = (const char *)dbsql_value_text(args[1]);

  if( !locale || !name ){
    return;
  }

  u_collator = ucol_open(locale, &status);
  if( !U_SUCCESS(status) ){
    __icu_function_error(p, "ucol_open", status);
    return;
  }
  assert(p);
  if(n_arg==3){
    const char *option = (const char*)dbsql_value_text(args[2]);
    static const struct {
       const char *name;
       UColAttributeValue val;
    } strengths[] = {
      {  "PRIMARY",      UCOL_PRIMARY           },
      {  "SECONDARY",    UCOL_SECONDARY         },
      {  "TERTIARY",     UCOL_TERTIARY          },
      {  "DEFAULT",      UCOL_DEFAULT_STRENGTH  },
      {  "QUARTERNARY",  UCOL_QUATERNARY        },
      {  "IDENTICAL",    UCOL_IDENTICAL         },
    };
    unsigned int i;
    for(i=0; i<sizeof(strengths)/sizeof(strengths[0]); i++){
      if( dbsql_stricmp(option,strengths[i].name)==0 ){
        ucol_setStrength(u_collator, strengths[i].val);
        break;
      }
    }
    if( i>=sizeof(strengths)/sizeof(strengths[0]) ){
      dbsql_str_t *str = dbsql_str_new(dbsql_context_db_handle(p));
      dbsql_str_appendf(str,
         "unknown collation strength \"%s\" - should be one of:",
         option);
      for(i=0; i<sizeof(strengths)/sizeof(strengths[0]); i++){
         dbsql_str_appendf(str, " %s", strengths[i].name);
      }
      dbsql_result_error(p, dbsql_str_value(str), -1);
      dbsql_free(dbsql_str_finish(str));
      ucol_close(u_collator);
      return;
    }
  }
  rc = dbsql_create_collation_v2(db, name, DBSQL_UTF16, (void *)u_collator, 
      __icu_collation_coll, __icu_collation_del
  );
  if( rc!=DBSQL_OK ){
    ucol_close(u_collator);
    dbsql_result_error(p, "Error registering collation function", -1);
  }
}

/*
** Register the ICU extension functions with database db.
*/
int __icu_init(DBSQL *db){
# define DBSQLICU_EXTRAFLAGS (DBSQL_DETERMINISTIC|DBSQL_INNOCUOUS)
  static const struct icu_scalar_t {
    const char *name;                        /* Function name */
    unsigned char n_arg;                       /* Number of arguments */
    unsigned int enc;                         /* Optimal text encoding */
    unsigned char i_context;                   /* dbsql_user_data() context */
    void (*func)(dbsql_func_t*,int,dbsql_value_t**);
  } scalars[] = {
    {"icu_load_collation",2,DBSQL_UTF8|DBSQL_DIRECTONLY,1, __icu_load_collation},
    {"icu_load_collation",3,DBSQL_UTF8|DBSQL_DIRECTONLY,1, __icu_load_collation},
#if !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_ICU)
    {"regexp", 2, DBSQL_ANY|DBSQLICU_EXTRAFLAGS,         0, __icu_regexp_func},
    {"lower",  1, DBSQL_UTF16|DBSQLICU_EXTRAFLAGS,       0, __icu_case_func16},
    {"lower",  2, DBSQL_UTF16|DBSQLICU_EXTRAFLAGS,       0, __icu_case_func16},
    {"upper",  1, DBSQL_UTF16|DBSQLICU_EXTRAFLAGS,       1, __icu_case_func16},
    {"upper",  2, DBSQL_UTF16|DBSQLICU_EXTRAFLAGS,       1, __icu_case_func16},
    {"lower",  1, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        0, __icu_case_func16},
    {"lower",  2, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        0, __icu_case_func16},
    {"upper",  1, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        1, __icu_case_func16},
    {"upper",  2, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        1, __icu_case_func16},
    {"like",   2, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        0, __icu_like_func},
    {"like",   3, DBSQL_UTF8|DBSQLICU_EXTRAFLAGS,        0, __icu_like_func},
#endif /* !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_ICU) */
  };
  int rc = DBSQL_OK;
  int i;
  
  for(i=0; rc==DBSQL_OK && i<(int)(sizeof(scalars)/sizeof(scalars[0])); i++){
    const struct icu_scalar_t *p = &scalars[i];
    rc = dbsql_create_function(
        db, p->name, p->n_arg, p->enc, 
        p->i_context ? (void*)db : (void*)0,
        p->func, 0, 0
    );
  }

  return rc;
}

#ifndef DBSQL_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_icu_init(
  DBSQL *db, 
  char **errmsg_p,
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api)
  return __icu_init(db);
}
#endif

#endif
