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
** This file contains C code that splits an SQL input string up into
** individual tokens and sends those tokens one-by-one over to the
** parser for analysis.
*/
#include "dbsql_int.h"
#include <stdlib.h>

/* Character classes for tokenizing
**
** In the __get_token() function, a switch() on __ai_class[c] is implemented
** using a lookup table, whereas a switch() directly on c uses a binary search.
** The lookup table is much faster.  To maximize speed, and to ensure that
** a lookup table is used, all of the classes need to be small integers and
** all of them need to be used within the switch.
*/
#define CC_X          0    /* The letter 'x', or start of BLOB literal */
#define CC_KYWD0      1    /* First letter of a keyword */
#define CC_KYWD       2    /* Alphabetics or '_'.  Usable in a keyword */
#define CC_DIGIT      3    /* Digits */
#define CC_DOLLAR     4    /* '$' */
#define CC_VARALPHA   5    /* '@', '#', ':'.  Alphabetic SQL variables */
#define CC_VARNUM     6    /* '?'.  Numeric SQL variables */
#define CC_SPACE      7    /* Space characters */
#define CC_QUOTE      8    /* '"', '\'', or '`'.  String literals, quoted ids */
#define CC_QUOTE2     9    /* '['.   [...] style quoted ids */
#define CC_PIPE      10    /* '|'.   Bitwise OR or concatenate */
#define CC_MINUS     11    /* '-'.  Minus or SQL-style comment */
#define CC_LT        12    /* '<'.  Part of < or <= or <> */
#define CC_GT        13    /* '>'.  Part of > or >= */
#define CC_EQ        14    /* '='.  Part of = or == */
#define CC_BANG      15    /* '!'.  Part of != */
#define CC_SLASH     16    /* '/'.  / or c-style comment */
#define CC_LP        17    /* '(' */
#define CC_RP        18    /* ')' */
#define CC_SEMI      19    /* ';' */
#define CC_PLUS      20    /* '+' */
#define CC_STAR      21    /* '*' */
#define CC_PERCENT   22    /* '%' */
#define CC_COMMA     23    /* ',' */
#define CC_AND       24    /* '&' */
#define CC_TILDA     25    /* '~' */
#define CC_DOT       26    /* '.' */
#define CC_ID        27    /* unicode characters usable in IDs */
#define CC_ILLEGAL   28    /* Illegal character */
#define CC_NUL       29    /* 0x00 */
#define CC_BOM       30    /* First byte of UTF8 BOM:  0xEF 0xBB 0xBF */

static const unsigned char __ai_class[] = {
#ifdef DBSQL_ASCII
/*         x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf */
/* 0x */   29, 28, 28, 28, 28, 28, 28, 28, 28,  7,  7, 28,  7,  7, 28, 28,
/* 1x */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 2x */    7, 15,  8,  5,  4, 22, 24,  8, 17, 18, 21, 20, 23, 11, 26, 16,
/* 3x */    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  5, 19, 12, 14, 13,  6,
/* 4x */    5,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 5x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2,  9, 28, 28, 28,  2,
/* 6x */    8,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 7x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2, 28, 10, 28, 25, 28,
/* 8x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* 9x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ax */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Bx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Cx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Dx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ex */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 30,
/* Fx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27
#endif
#ifdef DBSQL_EBCDIC
/*         x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf */
/* 0x */   29, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28,  7,  7, 28, 28,
/* 1x */   28, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 2x */   28, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 3x */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 4x */    7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 26, 12, 17, 20, 10,
/* 5x */   24, 28, 28, 28, 28, 28, 28, 28, 28, 28, 15,  4, 21, 18, 19, 28,
/* 6x */   11, 16, 28, 28, 28, 28, 28, 28, 28, 28, 28, 23, 22,  2, 13,  6,
/* 7x */   28, 28, 28, 28, 28, 28, 28, 28, 28,  8,  5,  5,  5,  8, 14,  8,
/* 8x */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* 9x */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Ax */   28, 25,  1,  1,  1,  1,  1,  0,  2,  2, 28, 28, 28, 28, 28, 28,
/* Bx */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28,  9, 28, 28, 28, 28, 28,
/* Cx */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Dx */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Ex */   28, 28,  1,  1,  1,  1,  1,  0,  2,  2, 28, 28, 28, 28, 28, 28,
/* Fx */    3,  3,  3,  3,  3,  3,  3,  3,  3,  3, 28, 28, 28, 28, 28, 28,
#endif
};

/*
** The CHAR_MAP() macro maps alphabetic characters (only) into their
** lower-case ASCII equivalent.  On ASCII machines, this is just
** an upper-to-lower case map.  On EBCDIC machines we also need
** to adjust the encoding.  The mapping is only valid for alphabetics
** which are the only characters for which this feature is used. 
**
** Used by keywordhash.h
*/
#ifdef DBSQL_ASCII
# define CHAR_MAP(X) __upper_to_lower[(unsigned char)X]
#endif
#ifdef DBSQL_EBCDIC
# define CHAR_MAP(X) __ebcdic_to_ascii[(unsigned char)X]
const unsigned char __ebcdic_to_ascii[] = {
/* 0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 0x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 1x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 2x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 3x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 4x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 5x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 95,  0,  0,  /* 6x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 7x */
   0, 97, 98, 99,100,101,102,103,104,105,  0,  0,  0,  0,  0,  0,  /* 8x */
   0,106,107,108,109,110,111,112,113,114,  0,  0,  0,  0,  0,  0,  /* 9x */
   0,  0,115,116,117,118,119,120,121,122,  0,  0,  0,  0,  0,  0,  /* Ax */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* Bx */
   0, 97, 98, 99,100,101,102,103,104,105,  0,  0,  0,  0,  0,  0,  /* Cx */
   0,106,107,108,109,110,111,112,113,114,  0,  0,  0,  0,  0,  0,  /* Dx */
   0,  0,115,116,117,118,119,120,121,122,  0,  0,  0,  0,  0,  0,  /* Ex */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* Fx */
};
#endif

/*
** The __keyword_code function looks up an identifier to determine if
** it is a keyword.  If it is a keyword, the token code of that keyword is 
** returned.  If the input is not a keyword, TK_ID is returned.
**
** The implementation of this routine was generated by a program,
** mkkeywordhash.c, located in the tool subdirectory of the distribution.
** The output of the mkkeywordhash.c program is written into a file
** named keywordhash.h and then included into this source file by
** the #include below.
*/
#include "keywordhash.h"


/*
** If X is a character that can be used in an identifier then
** ID_CHAR(X) will be true.  Otherwise it is false.
**
** For ASCII, any character with the high-order bit set is
** allowed in an identifier.  For 7-bit characters, 
** __is_id_char[X] must be 1.
**
** For EBCDIC, the rules are more complex but have the same
** end result.
**
** Ticket #1066.  the SQL standard does not allow '$' in the
** middle of identifiers.  But many SQL implementations do. 
** dbsql will allow '$' in identifiers for compatibility.
** But the feature is undocumented.
*/
#ifdef DBSQL_ASCII
#define ID_CHAR(C)  ((__ctype_map[(unsigned char)C]&0x46)!=0)
#endif
#ifdef DBSQL_EBCDIC
const char __is_ebcdic_id_char[] = {
/* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 a b c d e f */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 4x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0,  /* 5x */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0,  /* 6x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,  /* 7x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0,  /* 8x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0,  /* 9x */
    1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0,  /* Ax */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* Bx */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Cx */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Dx */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Ex */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0,  /* Fx */
};
#define ID_CHAR(C)  (((c=C)>=0x42 && __is_ebcdic_id_char[c-0x40]))
#endif

/* Make the ID_CHAR function accessible from ctime.c and alter.c */
int __is_id_char(u_int8_t c){ return ID_CHAR(c); }

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** Return the id of the next token in string (*pz). Before returning, set
** (*pz) to point to the byte following the parsed token.
*/
static int __get_token_local(const unsigned char **pz){
  const unsigned char *z = *pz;
  int t;                          /* Token type to return */
  do {
    z += __get_token(z, &t);
  }while( t==TK_SPACE || t==TK_COMMENT );
  if( t==TK_ID 
   || t==TK_STRING 
   || t==TK_JOIN_KW 
   || t==TK_WINDOW 
   || t==TK_OVER 
   || dbsql_parserFallback(t)==TK_ID 
  ){
    t = TK_ID;
  }
  *pz = z;
  return t;
}

/*
** The following three functions are called immediately after the tokenizer
** reads the keywords WINDOW, OVER and FILTER, respectively, to determine
** whether the token should be treated as a keyword or an SQL identifier.
** This cannot be handled by the usual lemon %fallback method, due to
** the ambiguity in some constructions. e.g.
**
**   SELECT sum(x) OVER ...
**
** In the above, "OVER" might be a keyword, or it might be an alias for the 
** sum(x) expression. If a "%fallback ID OVER" directive were added to 
** grammar, then dbsql would always treat "OVER" as an alias, making it
** impossible to call a window-function without a FILTER clause.
**
** WINDOW is treated as a keyword if:
**
**   * the following token is an identifier, or a keyword that can fallback
**     to being an identifier, and
**   * the token after than one is TK_AS.
**
** OVER is a keyword if:
**
**   * the previous token was TK_RP, and
**   * the next token is either TK_LP or an identifier.
**
** FILTER is a keyword if:
**
**   * the previous token was TK_RP, and
**   * the next token is TK_LP.
*/
static int __analyze_window_keyword(const unsigned char *z){
  int t;
  t = __get_token_local(&z);
  if( t!=TK_ID ) return TK_ID;
  t = __get_token_local(&z);
  if( t!=TK_AS ) return TK_ID;
  return TK_WINDOW;
}
static int __analyze_over_keyword(const unsigned char *z, int last_token){
  if( last_token==TK_RP ){
    int t = __get_token_local(&z);
    if( t==TK_LP || t==TK_ID ) return TK_OVER;
  }
  return TK_ID;
}
static int __analyze_filter_keyword(const unsigned char *z, int last_token){
  if( last_token==TK_RP && __get_token_local(&z)==TK_LP ){
    return TK_FILTER;
  }
  return TK_ID;
}
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** Return the length (in bytes) of the token that begins at z[0]. 
** Store the token type in *token_type before returning.
*/
int64_t __get_token(const unsigned char *z, int *token_type){
  int64_t i;
  int c;
  switch( __ai_class[*z] ){  /* Switch on the character-class of the first byte
                          ** of the token. See the comment on the CC_ defines
                          ** above. */
    case CC_SPACE: {
      testcase( z[0]==' ' );
      testcase( z[0]=='\t' );
      testcase( z[0]=='\n' );
      testcase( z[0]=='\f' );
      testcase( z[0]=='\r' );
      for(i=1; __isspace(z[i]); i++){}
      *token_type = TK_SPACE;
      return i;
    }
    case CC_MINUS: {
      if( z[1]=='-' ){
        for(i=2; (c=z[i])!=0 && c!='\n'; i++){}
        *token_type = TK_COMMENT;
        return i;
      }else if( z[1]=='>' ){
        *token_type = TK_PTR;
        return 2 + (z[2]=='>');
      }
      *token_type = TK_MINUS;
      return 1;
    }
    case CC_LP: {
      *token_type = TK_LP;
      return 1;
    }
    case CC_RP: {
      *token_type = TK_RP;
      return 1;
    }
    case CC_SEMI: {
      *token_type = TK_SEMI;
      return 1;
    }
    case CC_PLUS: {
      *token_type = TK_PLUS;
      return 1;
    }
    case CC_STAR: {
      *token_type = TK_STAR;
      return 1;
    }
    case CC_SLASH: {
      if( z[1]!='*' || z[2]==0 ){
        *token_type = TK_SLASH;
        return 1;
      }
      for(i=3, c=z[2]; (c!='*' || z[i]!='/') && (c=z[i])!=0; i++){}
      if( c ) i++;
      *token_type = TK_COMMENT;
      return i;
    }
    case CC_PERCENT: {
      *token_type = TK_REM;
      return 1;
    }
    case CC_EQ: {
      *token_type = TK_EQ;
      return 1 + (z[1]=='=');
    }
    case CC_LT: {
      if( (c=z[1])=='=' ){
        *token_type = TK_LE;
        return 2;
      }else if( c=='>' ){
        *token_type = TK_NE;
        return 2;
      }else if( c=='<' ){
        *token_type = TK_LSHIFT;
        return 2;
      }else{
        *token_type = TK_LT;
        return 1;
      }
    }
    case CC_GT: {
      if( (c=z[1])=='=' ){
        *token_type = TK_GE;
        return 2;
      }else if( c=='>' ){
        *token_type = TK_RSHIFT;
        return 2;
      }else{
        *token_type = TK_GT;
        return 1;
      }
    }
    case CC_BANG: {
      if( z[1]!='=' ){
        *token_type = TK_ILLEGAL;
        return 1;
      }else{
        *token_type = TK_NE;
        return 2;
      }
    }
    case CC_PIPE: {
      if( z[1]!='|' ){
        *token_type = TK_BITOR;
        return 1;
      }else{
        *token_type = TK_CONCAT;
        return 2;
      }
    }
    case CC_COMMA: {
      *token_type = TK_COMMA;
      return 1;
    }
    case CC_AND: {
      *token_type = TK_BITAND;
      return 1;
    }
    case CC_TILDA: {
      *token_type = TK_BITNOT;
      return 1;
    }
    case CC_QUOTE: {
      int delim = z[0];
      testcase( delim=='`' );
      testcase( delim=='\'' );
      testcase( delim=='"' );
      for(i=1; (c=z[i])!=0; i++){
        if( c==delim ){
          if( z[i+1]==delim ){
            i++;
          }else{
            break;
          }
        }
      }
      if( c=='\'' ){
        *token_type = TK_STRING;
        return i+1;
      }else if( c!=0 ){
        *token_type = TK_ID;
        return i+1;
      }else{
        *token_type = TK_ILLEGAL;
        return i;
      }
    }
    case CC_DOT: {
#ifndef DBSQL_OMIT_FLOATING_POINT
      if( !__isdigit(z[1]) )
#endif
      {
        *token_type = TK_DOT;
        return 1;
      }
      /* If the next character is a digit, this is a floating point
      ** number that begins with ".".  Fall thru into the next case */
      /* no break */ deliberate_fall_through
    }
    case CC_DIGIT: {
      testcase( z[0]=='0' );  testcase( z[0]=='1' );  testcase( z[0]=='2' );
      testcase( z[0]=='3' );  testcase( z[0]=='4' );  testcase( z[0]=='5' );
      testcase( z[0]=='6' );  testcase( z[0]=='7' );  testcase( z[0]=='8' );
      testcase( z[0]=='9' );  testcase( z[0]=='.' );
      *token_type = TK_INTEGER;
#ifndef DBSQL_OMIT_HEX_INTEGER
      if( z[0]=='0' && (z[1]=='x' || z[1]=='X') && __isxdigit(z[2]) ){
        for(i=3; 1; i++){
          if( __isxdigit(z[i])==0 ){
            if( z[i]==DBSQL_DIGIT_SEPARATOR ){
              *token_type = TK_QNUMBER;
            }else{
              break;
            }
          }
        }
      }else
#endif
        {
        for(i=0; 1; i++){
          if( __isdigit(z[i])==0 ){
            if( z[i]==DBSQL_DIGIT_SEPARATOR ){
              *token_type = TK_QNUMBER;
            }else{
              break;
            }
          }
        }
#ifndef DBSQL_OMIT_FLOATING_POINT
        if( z[i]=='.' ){
          if( *token_type==TK_INTEGER ) *token_type = TK_FLOAT;
          for(i++; 1; i++){
            if( __isdigit(z[i])==0 ){
              if( z[i]==DBSQL_DIGIT_SEPARATOR ){
                *token_type = TK_QNUMBER;
              }else{
                break;
              }
            }
          }
        }
        if( (z[i]=='e' || z[i]=='E') &&
             ( __isdigit(z[i+1]) 
              || ((z[i+1]=='+' || z[i+1]=='-') && __isdigit(z[i+2]))
             )
        ){
          if( *token_type==TK_INTEGER ) *token_type = TK_FLOAT;
          for(i+=2; 1; i++){
            if( __isdigit(z[i])==0 ){
              if( z[i]==DBSQL_DIGIT_SEPARATOR ){
                *token_type = TK_QNUMBER;
              }else{
                break;
              }
            }
          }
        }
#endif
      }
      while( ID_CHAR(z[i]) ){
        *token_type = TK_ILLEGAL;
        i++;
      }
      return i;
    }
    case CC_QUOTE2: {
      for(i=1, c=z[0]; c!=']' && (c=z[i])!=0; i++){}
      *token_type = c==']' ? TK_ID : TK_ILLEGAL;
      return i;
    }
    case CC_VARNUM: {
      *token_type = TK_VARIABLE;
      for(i=1; __isdigit(z[i]); i++){}
      return i;
    }
    case CC_DOLLAR:
    case CC_VARALPHA: {
      int64_t n = 0;
      testcase( z[0]=='$' );  testcase( z[0]=='@' );
      testcase( z[0]==':' );  testcase( z[0]=='#' );
      *token_type = TK_VARIABLE;
      for(i=1; (c=z[i])!=0; i++){
        if( ID_CHAR(c) ){
          n++;
#ifndef DBSQL_OMIT_TCL_VARIABLE
        }else if( c=='(' && n>0 ){
          do{
            i++;
          }while( (c=z[i])!=0 && !__isspace(c) && c!=')' );
          if( c==')' ){
            i++;
          }else{
            *token_type = TK_ILLEGAL;
          }
          break;
        }else if( c==':' && z[i+1]==':' ){
          i++;
#endif
        }else{
          break;
        }
      }
      if( n==0 ) *token_type = TK_ILLEGAL;
      return i;
    }
    case CC_KYWD0: {
      if( __ai_class[z[1]]>CC_KYWD ){ i = 1;  break; }
      for(i=2; __ai_class[z[i]]<=CC_KYWD; i++){}
      if( ID_CHAR(z[i]) ){
        /* This token started out using characters that can appear in keywords,
        ** but z[i] is a character not allowed within keywords, so this must
        ** be an identifier instead */
        i++;
        break;
      }
      *token_type = TK_ID;
      return __keyword_code_local((char*)z, i, token_type);
    }
    case CC_X: {
#ifndef DBSQL_OMIT_BLOB_LITERAL
      testcase( z[0]=='x' ); testcase( z[0]=='X' );
      if( z[1]=='\'' ){
        *token_type = TK_BLOB;
        for(i=2; __isxdigit(z[i]); i++){}
        if( z[i]!='\'' || i%2 ){
          *token_type = TK_ILLEGAL;
          while( z[i] && z[i]!='\'' ){ i++; }
        }
        if( z[i] ) i++;
        return i;
      }
#endif
      /* If it is not a BLOB literal, then it must be an ID, since no
      ** SQL keywords start with the letter 'x'.  Fall through */
      /* no break */ deliberate_fall_through
    }
    case CC_KYWD:
    case CC_ID: {
      i = 1;
      break;
    }
    case CC_BOM: {
      if( z[1]==0xbb && z[2]==0xbf ){
        *token_type = TK_SPACE;
        return 3;
      }
      i = 1;
      break;
    }
    case CC_NUL: {
      *token_type = TK_ILLEGAL;
      return 0;
    }
    default: {
      *token_type = TK_ILLEGAL;
      return 1;
    }
  }
  while( ID_CHAR(z[i]) ){ i++; }
  *token_type = TK_ID;
  return i;
}

/*
** Run the parser on the given SQL string.
*/
int __run_parser(parser_t *parser, const char *sql){
  int n_err = 0;                   /* Number of errors encountered */
  void *engine;                  /* The LEMON-generated LALR(1) parser */
  int64_t n = 0;                      /* Length of the next token token */
  int token_type;                  /* type of the next token */
  int last_token_parsed = -1;       /* type of the previous token */
  DBSQL *db = parser->db;       /* The database connection */
  int64_t mx_sql_len;                   /* Max length of an SQL string */
  parser_t *parent_parse = 0;        /* Outer parse context, if any */
#ifdef dbsql_parser_ENGINEALWAYSONSTACK
  yyParser s_engine;    /* Space to hold the Lemon-generated Parser object */
#endif
  VVA_ONLY( u_int8_t started_with_oom = db->malloc_failed );

  assert( sql!=0 );
  mx_sql_len = db->limits[DBSQL_LIMIT_SQL_LENGTH];
  if( db->n_vdbe_active==0 ){
    ATOMIC_STORE(&db->u1.is_interrupted, 0);
  }
  parser->rc = DBSQL_OK;
  parser->tail = sql;
#ifdef DBSQL_DEBUG
  if( db->flags & DBSQL_ParserTrace ){
    printf("parser: [[[%s]]]\n", sql);
    dbsql_parserTrace(stdout, "parser: ");
  }else{
    dbsql_parserTrace(0, 0);
  }
#endif
#ifdef dbsql_parser_ENGINEALWAYSONSTACK
  engine = &s_engine;
  dbsql_parserInit(engine, parser);
#else
  engine = dbsql_parserAlloc(__dbsql_malloc, parser);
  if( engine==0 ){
    __oom_fault(db);
    return DBSQL_NOMEM_BKPT;
  }
#endif
  assert( parser->new_table==0 );
  assert( parser->new_trigger==0 );
  assert( parser->n_var==0 );
  assert( parser->v_list==0 );
  parent_parse = db->parser;
  db->parser = parser;
  while( 1 ){
    n = __get_token((u_int8_t*)sql, &token_type);
    mx_sql_len -= n;
    if( mx_sql_len<0 ){
      parser->rc = DBSQL_TOOBIG;
      parser->n_err++;
      break;
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( token_type>=TK_WINDOW ){
      assert( token_type==TK_SPACE || token_type==TK_OVER || token_type==TK_FILTER
           || token_type==TK_ILLEGAL || token_type==TK_WINDOW 
           || token_type==TK_QNUMBER || token_type==TK_COMMENT
      );
#else
    if( token_type>=TK_SPACE ){
      assert( token_type==TK_SPACE || token_type==TK_ILLEGAL 
           || token_type==TK_QNUMBER || token_type==TK_COMMENT
      );
#endif /* DBSQL_OMIT_WINDOWFUNC */
      if( ATOMIC_LOAD(&db->u1.is_interrupted) ){
        parser->rc = DBSQL_INTERRUPT;
        parser->n_err++;
        break;
      }
      if( token_type==TK_SPACE ){
        sql += n;
        continue;
      }
      if( sql[0]==0 ){
        /* Upon reaching the end of input, call the parser two more times
        ** with tokens TK_SEMI and 0, in that order. */
        if( last_token_parsed==TK_SEMI ){
          token_type = 0;
        }else if( last_token_parsed==0 ){
          break;
        }else{
          token_type = TK_SEMI;
        }
        n = 0;
#ifndef DBSQL_OMIT_WINDOWFUNC
      }else if( token_type==TK_WINDOW ){
        assert( n==6 );
        token_type = __analyze_window_keyword((const u_int8_t*)&sql[6]);
      }else if( token_type==TK_OVER ){
        assert( n==4 );
        token_type = __analyze_over_keyword((const u_int8_t*)&sql[4], last_token_parsed);
      }else if( token_type==TK_FILTER ){
        assert( n==6 );
        token_type = __analyze_filter_keyword((const u_int8_t*)&sql[6], last_token_parsed);
#endif /* DBSQL_OMIT_WINDOWFUNC */
      }else if( token_type==TK_COMMENT
             && (db->init.busy || (db->flags & DBSQL_Comments)!=0)
      ){
        /* Ignore SQL comments if either (1) we are reparsing the schema or
        ** (2) DBSQL_DBCONFIG_ENABLE_COMMENTS is turned on (the default). */
        sql += n;
        continue;
      }else if( token_type!=TK_QNUMBER ){
        Token x;
        x.z = sql;
        x.n = (u_int32_t)n;
        __error_msg(parser, "unrecognized token: \"%T\"", &x);
        break;
      }
    }
    parser->s_last_token.z = sql;
    parser->s_last_token.n = (u_int32_t)n;
    dbsql_parser(engine, token_type, parser->s_last_token);
    last_token_parsed = token_type;
    sql += n;
    assert( db->malloc_failed==0 || parser->rc!=DBSQL_OK || started_with_oom );
    if( parser->rc!=DBSQL_OK ) break;
  }
  assert( n_err==0 );
#ifdef YYTRACKMAXSTACKDEPTH
  dbsql_mutex_enter(__malloc_mutex());
  __status_highwater(DBSQL_STATUS_PARSER_STACK,
      dbsql_parserStackPeak(engine)
  );
  dbsql_mutex_leave(__malloc_mutex());
#endif /* YYDEBUG */
#ifdef dbsql_parser_ENGINEALWAYSONSTACK
  dbsql_parserFinalize(engine);
#else
  dbsql_parserFree(engine, dbsql_free);
#endif
  if( db->malloc_failed ){
    parser->rc = DBSQL_NOMEM_BKPT;
  }
  if( parser->errmsg || (parser->rc!=DBSQL_OK && parser->rc!=DBSQL_DONE) ){
    if( parser->errmsg==0 ){
      parser->errmsg = __db_str_dup(db, __err_str(parser->rc));
    }
    if( (parser->prep_flags & DBSQL_PREPARE_DONT_LOG)==0 ){
      dbsql_log(parser->rc, "%s in \"%s\"", parser->errmsg, parser->tail);
    }
    n_err++;
  }
  parser->tail = sql;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_free(parser->vtab_locks);
#endif

  if( parser->new_table && !IN_SPECIAL_PARSE ){
    /* If the parser->declare_vtab flag is set, do not delete any table 
    ** structure built up in parser->new_table. The calling code (see vtab.c)
    ** will take responsibility for freeing the table_t structure.
    */
    __delete_table(db, parser->new_table);
  }
  if( parser->new_trigger && !IN_RENAME_OBJECT ){
    __delete_trigger(db, parser->new_trigger);
  }
  if( parser->v_list ) __db_nn_free_nn(db, parser->v_list);
  db->parser = parent_parse;
  assert( n_err==0 || parser->rc!=DBSQL_OK );
  return n_err;
}


#ifdef DBSQL_ENABLE_NORMALIZE
/*
** Insert a single space character into str if the current string
** ends with an identifier
*/
static void add_space_separator(dbsql_str_t *str){
  if( str->n_char && __is_id_char(str->text[str->n_char-1]) ){
    dbsql_str_append(str, " ", 1);
  }
}

/*
** Compute a normalization of the SQL given by sql[0..n_sql-1].  Return
** the normalization in space obtained from __db_malloc().  Or return
** NULL if anything goes wrong or if sql is NULL.
*/
char *__normalize(
  vdbe_t *vdbe,       /* VM being reprepared */
  const char *sql   /* The original SQL string */
){
  DBSQL *db;       /* The database connection */
  int i;             /* Next unread byte of sql[] */
  int64_t n;             /* length of current token */
  int token_type;     /* type of current token */
  int prev_type = 0;  /* Previous non-whitespace token */
  int n_paren;        /* Number of nested levels of parentheses */
  int i_start_in;      /* Start of RHS of IN operator in z[] */
  int n_paren_at_in;    /* Value of n_parent at start of RHS of IN operator */
  u_int32_t j;             /* Bytes of normalized SQL generated so far */
  dbsql_str_t *str; /* The normalized SQL string under construction */

  db = __vdbe_db(vdbe);
  token_type = -1;
  n_paren = i_start_in = n_paren_at_in = 0;
  str = dbsql_str_new(db);
  assert( str!=0 );  /* dbsql_str_new() never returns NULL */
  for(i=0; sql[i] && str->acc_error==0; i+=n){
    if( token_type!=TK_SPACE ){
      prev_type = token_type;
    }
    n = __get_token((unsigned char*)sql+i, &token_type);
    if( NEVER(n<=0) ) break;
    switch( token_type ){
      case TK_COMMENT:
      case TK_SPACE: {
        break;
      }
      case TK_NULL: {
        if( prev_type==TK_IS || prev_type==TK_NOT ){
          dbsql_str_append(str, " NULL", 5);
          break;
        }
        /* no break */ deliberate_fall_through
      }
      case TK_STRING:
      case TK_INTEGER:
      case TK_FLOAT:
      case TK_VARIABLE:
      case TK_BLOB: {
        dbsql_str_append(str, "?", 1);
        break;
      }
      case TK_LP: {
        n_paren++;
        if( prev_type==TK_IN ){
          i_start_in = str->n_char;
          n_paren_at_in = n_paren;
        }
        dbsql_str_append(str, "(", 1);
        break;
      }
      case TK_RP: {
        if( i_start_in>0 && n_paren==n_paren_at_in ){
          assert( str->n_char>=(u_int32_t)i_start_in );
          str->n_char = i_start_in+1;
          dbsql_str_append(str, "?,?,?", 5);
          i_start_in = 0;
        }
        n_paren--;
        dbsql_str_append(str, ")", 1);
        break;
      }
      case TK_ID: {
        i_start_in = 0;
        j = str->n_char;
        if( __isquote(sql[i]) ){
          char *id = __db_str_n_dup(db, sql+i, n);
          int n_id;
          int type = 0;
          if( id==0 ) break;
          __dequote(id);
          if( sql[i]=='"' && __vdbe_uses_double_quoted_string(vdbe, id) ){
            dbsql_str_append(str, "?", 1);
            __dbsql_db_free(db, id);
            break;
          }
          n_id = __strlen30(id);
          if( __get_token((u_int8_t*)id, &type)==n_id && type==TK_ID ){
            add_space_separator(str);
            dbsql_str_append(str, id, n_id);
          }else{
            dbsql_str_appendf(str, "\"%w\"", id);
          }
          __dbsql_db_free(db, id);
        }else{
          add_space_separator(str);
          dbsql_str_append(str, sql+i, n);
        }
        while( j<str->n_char ){
          str->text[j] = __dbsql_tolower(str->text[j]);
          j++;
        }
        break;
      }
      case TK_SELECT: {
        i_start_in = 0;
        /* no break */ deliberate_fall_through
      }
      default: {
        if( __is_id_char(sql[i]) ) add_space_separator(str);
        j = str->n_char;
        dbsql_str_append(str, sql+i, n);
        while( j<str->n_char ){
          str->text[j] = __dbsql_toupper(str->text[j]);
          j++;
        }
        break;
      }
    }
  }
  if( token_type!=TK_SEMI ) dbsql_str_append(str, ";", 1);
  return dbsql_str_finish(str);
}
#endif /* DBSQL_ENABLE_NORMALIZE */
