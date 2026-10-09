/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** An tokenizer for SQL
**
** This file contains C code that implements the dbsql_complete() API.
** This code used to be part of the tokenizer.c source file.  But by
** separating it out, the code will be automatically omitted from
** static links that do not use it.
*/
#include "dbsql_int.h"
#ifndef DBSQL_OMIT_COMPLETE

/*
** This is defined in tokenize.c.  We just have to import the definition.
*/
#ifndef DBSQL_AMALGAMATION
#ifdef DBSQL_ASCII
#define ID_CHAR(C)  ((__ctype_map[(unsigned char)C]&0x46)!=0)
#endif
#ifdef DBSQL_EBCDIC
extern const char __is_ebcdic_id_char[];
#define ID_CHAR(C)  (((c=C)>=0x42 && __is_ebcdic_id_char[c-0x40]))
#endif
#endif /* DBSQL_AMALGAMATION */


/*
** Token types used by the dbsql_complete() routine.  See the header
** comments on that procedure for additional information.
*/
#define TK_SEMI    0
#define TK_WS      1
#define TK_OTHER   2
#ifndef DBSQL_OMIT_TRIGGER
#define TK_EXPLAIN 3
#define TK_CREATE  4
#define TK_TEMP    5
#define TK_TRIGGER 6
#define TK_END     7
#endif

/*
** Return TRUE if the given SQL string ends in a semicolon.
**
** Special handling is require for CREATE TRIGGER statements.
** Whenever the CREATE TRIGGER keywords are seen, the statement
** must end with ";END;".
**
** This implementation uses a state machine with 8 states:
**
**   (0) INVALID   We have not yet seen a non-whitespace character.
**
**   (1) START     At the beginning or end of an SQL statement.  This routine
**                 returns 1 if it ends in the START state and 0 if it ends
**                 in any other state.
**
**   (2) NORMAL    We are in the middle of statement which ends with a single
**                 semicolon.
**
**   (3) EXPLAIN   The keyword EXPLAIN has been seen at the beginning of 
**                 a statement.
**
**   (4) CREATE    The keyword CREATE has been seen at the beginning of a
**                 statement, possibly preceded by EXPLAIN and/or followed by
**                 TEMP or TEMPORARY
**
**   (5) TRIGGER   We are in the middle of a trigger definition that must be
**                 ended by a semicolon, the keyword END, and another semicolon.
**
**   (6) SEMI      We've seen the first semicolon in the ";END;" that occurs at
**                 the end of a trigger definition.
**
**   (7) END       We've seen the ";END" of the ";END;" that occurs at the end
**                 of a trigger definition.
**
** Transitions between states above are determined by tokens extracted
** from the input.  The following tokens are significant:
**
**   (0) TK_SEMI      A semicolon.
**   (1) TK_WS        Whitespace.
**   (2) TK_OTHER     Any other SQL token.
**   (3) TK_EXPLAIN   The "explain" keyword.
**   (4) TK_CREATE    The "create" keyword.
**   (5) TK_TEMP      The "temp" or "temporary" keyword.
**   (6) TK_TRIGGER   The "trigger" keyword.
**   (7) TK_END       The "end" keyword.
**
** Whitespace never causes a state transition and is always ignored.
** This means that a SQL string of all whitespace is invalid.
**
** If we compile with DBSQL_OMIT_TRIGGER, all of the computation needed
** to recognize the end of a trigger can be omitted.  All we have to do
** is look for a semicolon that is not part of an string or comment.
*/
int dbsql_complete(const char *sql){
  u_int8_t state = 0;   /* Current state, using numbers defined in header comment */
  u_int8_t token;       /* Value of the next token */

#ifndef DBSQL_OMIT_TRIGGER
  /* A complex statement machine used to detect the end of a CREATE TRIGGER
  ** statement.  This is the normal case.
  */
  static const u_int8_t trans[8][8] = {
                     /* Token:                                                */
     /* State:       **  SEMI  WS  OTHER  EXPLAIN  CREATE  TEMP  TRIGGER  END */
     /* 0 INVALID: */ {    1,  0,     2,       3,      4,    2,       2,   2, },
     /* 1   START: */ {    1,  1,     2,       3,      4,    2,       2,   2, },
     /* 2  NORMAL: */ {    1,  2,     2,       2,      2,    2,       2,   2, },
     /* 3 EXPLAIN: */ {    1,  3,     3,       2,      4,    2,       2,   2, },
     /* 4  CREATE: */ {    1,  4,     2,       2,      2,    4,       5,   2, },
     /* 5 TRIGGER: */ {    6,  5,     5,       5,      5,    5,       5,   5, },
     /* 6    SEMI: */ {    6,  6,     5,       5,      5,    5,       5,   7, },
     /* 7     END: */ {    1,  7,     5,       5,      5,    5,       5,   5, },
  };
#else
  /* If triggers are not supported by this compile then the statement machine
  ** used to detect the end of a statement is much simpler
  */
  static const u_int8_t trans[3][3] = {
                     /* Token:           */
     /* State:       **  SEMI  WS  OTHER */
     /* 0 INVALID: */ {    1,  0,     2, },
     /* 1   START: */ {    1,  1,     2, },
     /* 2  NORMAL: */ {    1,  2,     2, },
  };
#endif /* DBSQL_OMIT_TRIGGER */

#ifdef DBSQL_ENABLE_API_ARMOR
  if( sql==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif

  while( *sql ){
    switch( *sql ){
      case ';': {  /* A semicolon */
        token = TK_SEMI;
        break;
      }
      case ' ':
      case '\r':
      case '\t':
      case '\n':
      case '\f': {  /* White space is ignored */
        token = TK_WS;
        break;
      }
      case '/': {   /* C-style comments */
        if( sql[1]!='*' ){
          token = TK_OTHER;
          break;
        }
        sql += 2;
        while( sql[0] && (sql[0]!='*' || sql[1]!='/') ){ sql++; }
        if( sql[0]==0 ) return 0;
        sql++;
        token = TK_WS;
        break;
      }
      case '-': {   /* SQL-style comments from "--" to end of line */
        if( sql[1]!='-' ){
          token = TK_OTHER;
          break;
        }
        while( *sql && *sql!='\n' ){ sql++; }
        if( *sql==0 ) return state==1;
        token = TK_WS;
        break;
      }
      case '[': {   /* Microsoft-style identifiers in [...] */
        sql++;
        while( *sql && *sql!=']' ){ sql++; }
        if( *sql==0 ) return 0;
        token = TK_OTHER;
        break;
      }
      case '`':     /* Grave-accent quoted symbols used by __my_sql */
      case '"':     /* single- and double-quoted strings */
      case '\'': {
        int c = *sql;
        sql++;
        while( *sql && *sql!=c ){ sql++; }
        if( *sql==0 ) return 0;
        token = TK_OTHER;
        break;
      }
      default: {
#ifdef DBSQL_EBCDIC
        unsigned char c;
#endif
        if( ID_CHAR((u_int8_t)*sql) ){
          /* Keywords and unquoted identifiers */
          int n_id;
          for(n_id=1; ID_CHAR(sql[n_id]); n_id++){}
#ifdef DBSQL_OMIT_TRIGGER
          token = TK_OTHER;
#else
          switch( *sql ){
            case 'c': case 'C': {
              if( n_id==6 && __str_nicmp(sql, "create", 6)==0 ){
                token = TK_CREATE;
              }else{
                token = TK_OTHER;
              }
              break;
            }
            case 't': case 'T': {
              if( n_id==7 && __str_nicmp(sql, "trigger", 7)==0 ){
                token = TK_TRIGGER;
              }else if( n_id==4 && __str_nicmp(sql, "temp", 4)==0 ){
                token = TK_TEMP;
              }else if( n_id==9 && __str_nicmp(sql, "temporary", 9)==0 ){
                token = TK_TEMP;
              }else{
                token = TK_OTHER;
              }
              break;
            }
            case 'e':  case 'E': {
              if( n_id==3 && __str_nicmp(sql, "end", 3)==0 ){
                token = TK_END;
              }else
#ifndef DBSQL_OMIT_EXPLAIN
              if( n_id==7 && __str_nicmp(sql, "explain", 7)==0 ){
                token = TK_EXPLAIN;
              }else
#endif
              {
                token = TK_OTHER;
              }
              break;
            }
            default: {
              token = TK_OTHER;
              break;
            }
          }
#endif /* DBSQL_OMIT_TRIGGER */
          sql += n_id-1;
        }else{
          /* Operators and special symbols */
          token = TK_OTHER;
        }
        break;
      }
    }
    state = trans[state][token];
    sql++;
  }
  return state==1;
}

#ifndef DBSQL_OMIT_UTF16
/*
** This routine is the same as the dbsql_complete() routine described
** above, except that the parameter is required to be UTF-16 encoded, not
** UTF-8.
*/
int dbsql_complete16(const void *sql){
  dbsql_value_t *val;
  char const *sql8;
  int rc;

#ifndef DBSQL_OMIT_AUTOINIT
  rc = dbsql_initialize();
  if( rc ) return rc;
#endif
  val = __value_new(0);
  __value_set_str(val, -1, sql, DBSQL_UTF16NATIVE, DBSQL_STATIC);
  sql8 = __value_text(val, DBSQL_UTF8);
  if( sql8 ){
    rc = dbsql_complete(sql8);
  }else{
    rc = DBSQL_NOMEM_BKPT;
  }
  __value_free(val);
  return rc & 0xff;
}
#endif /* DBSQL_OMIT_UTF16 */
#endif /* DBSQL_OMIT_COMPLETE */
