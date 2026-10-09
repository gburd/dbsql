/*
** 2012 April 10
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
** This module implements the spellfix1 VIRTUAL TABLE that can be used
** to search a large vocabulary for close matches.  See separate
** documentation (http://sqlite.org/spellfix1.html) for details.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1

#ifndef DBSQL_AMALGAMATION
# if !defined(NDEBUG) && !defined(DBSQL_DEBUG)
#  define NDEBUG 1
# endif
# if defined(NDEBUG) && defined(DBSQL_DEBUG)
#  undef NDEBUG
# endif
# include <string.h>
# include <stdio.h>
# include <stdlib.h>
# include <assert.h>
  typedef unsigned char u_int8_t;
  typedef unsigned short u_int16_t;
  typedef int64_t int64_t;
#endif
#include <ctype.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Character classes for ASCII characters:
**
**   0   ''        Silent letters:   H W
**   1   'A'       Any vowel:   A E I O U (Y)
**   2   'B'       A bilabeal stop or fricative:  B F P V W
**   3   'C'       Other fricatives or back stops:  C G J K Q S X Z
**   4   'D'       Alveolar stops:  D T
**   5   'H'       Letter H at the beginning of a word
**   6   'L'       Glide:  L
**   7   'R'       Semivowel:  R
**   8   'M'       Nasals:  M N
**   9   'Y'       Letter Y at the beginning of a word.
**   10  '9'       Digits: 0 1 2 3 4 5 6 7 8 9
**   11  ' '       White space
**   12  '?'       Other.
*/
#define CCLASS_SILENT         0
#define CCLASS_VOWEL          1
#define CCLASS_B              2
#define CCLASS_C              3
#define CCLASS_D              4
#define CCLASS_H              5
#define CCLASS_L              6
#define CCLASS_R              7
#define CCLASS_M              8
#define CCLASS_Y              9
#define CCLASS_DIGIT         10
#define CCLASS_SPACE         11
#define CCLASS_OTHER         12

/*
** The following table gives the character class for non-initial ASCII
** characters.
*/
static const unsigned char __mid_class[] = {
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_SPACE,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_SPACE,    /*   */ CCLASS_SPACE,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_SPACE,
 /* ! */ CCLASS_OTHER,    /* " */ CCLASS_OTHER,   /* # */ CCLASS_OTHER,
 /* $ */ CCLASS_OTHER,    /* % */ CCLASS_OTHER,   /* & */ CCLASS_OTHER,
 /* ' */ CCLASS_SILENT,   /* ( */ CCLASS_OTHER,   /* ) */ CCLASS_OTHER,
 /* * */ CCLASS_OTHER,    /* + */ CCLASS_OTHER,   /* , */ CCLASS_OTHER,
 /* - */ CCLASS_OTHER,    /* . */ CCLASS_OTHER,   /* / */ CCLASS_OTHER,
 /* 0 */ CCLASS_DIGIT,    /* 1 */ CCLASS_DIGIT,   /* 2 */ CCLASS_DIGIT,
 /* 3 */ CCLASS_DIGIT,    /* 4 */ CCLASS_DIGIT,   /* 5 */ CCLASS_DIGIT,
 /* 6 */ CCLASS_DIGIT,    /* 7 */ CCLASS_DIGIT,   /* 8 */ CCLASS_DIGIT,
 /* 9 */ CCLASS_DIGIT,    /* : */ CCLASS_OTHER,   /* ; */ CCLASS_OTHER,
 /* < */ CCLASS_OTHER,    /* = */ CCLASS_OTHER,   /* > */ CCLASS_OTHER,
 /* ? */ CCLASS_OTHER,    /* @ */ CCLASS_OTHER,   /* A */ CCLASS_VOWEL,
 /* B */ CCLASS_B,        /* C */ CCLASS_C,       /* D */ CCLASS_D,
 /* E */ CCLASS_VOWEL,    /* F */ CCLASS_B,       /* G */ CCLASS_C,
 /* H */ CCLASS_SILENT,   /* I */ CCLASS_VOWEL,   /* J */ CCLASS_C,
 /* K */ CCLASS_C,        /* L */ CCLASS_L,       /* M */ CCLASS_M,
 /* N */ CCLASS_M,        /* O */ CCLASS_VOWEL,   /* P */ CCLASS_B,
 /* Q */ CCLASS_C,        /* R */ CCLASS_R,       /* S */ CCLASS_C,
 /* T */ CCLASS_D,        /* U */ CCLASS_VOWEL,   /* V */ CCLASS_B,
 /* W */ CCLASS_B,        /* X */ CCLASS_C,       /* Y */ CCLASS_VOWEL,
 /* Z */ CCLASS_C,        /* [ */ CCLASS_OTHER,   /* \ */ CCLASS_OTHER,
 /* ] */ CCLASS_OTHER,    /* ^ */ CCLASS_OTHER,   /* _ */ CCLASS_OTHER,
 /* ` */ CCLASS_OTHER,    /* a */ CCLASS_VOWEL,   /* b */ CCLASS_B,
 /* c */ CCLASS_C,        /* d */ CCLASS_D,       /* e */ CCLASS_VOWEL,
 /* f */ CCLASS_B,        /* g */ CCLASS_C,       /* h */ CCLASS_SILENT,
 /* i */ CCLASS_VOWEL,    /* j */ CCLASS_C,       /* k */ CCLASS_C,
 /* l */ CCLASS_L,        /* m */ CCLASS_M,       /* n */ CCLASS_M,
 /* o */ CCLASS_VOWEL,    /* p */ CCLASS_B,       /* q */ CCLASS_C,
 /* r */ CCLASS_R,        /* s */ CCLASS_C,       /* t */ CCLASS_D,
 /* u */ CCLASS_VOWEL,    /* v */ CCLASS_B,       /* w */ CCLASS_B,
 /* x */ CCLASS_C,        /* y */ CCLASS_VOWEL,   /* z */ CCLASS_C,
 /* { */ CCLASS_OTHER,    /* | */ CCLASS_OTHER,   /* } */ CCLASS_OTHER,
 /* ~ */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   
};
/* 
** This tables gives the character class for ASCII characters that form the
** initial character of a word.  The only difference from __mid_class is with
** the letters H, W, and Y.
*/
static const unsigned char __init_class[] = {
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_SPACE,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_SPACE,    /*   */ CCLASS_SPACE,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_OTHER,
 /*   */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   /*   */ CCLASS_SPACE,
 /* ! */ CCLASS_OTHER,    /* " */ CCLASS_OTHER,   /* # */ CCLASS_OTHER,
 /* $ */ CCLASS_OTHER,    /* % */ CCLASS_OTHER,   /* & */ CCLASS_OTHER,
 /* ' */ CCLASS_OTHER,    /* ( */ CCLASS_OTHER,   /* ) */ CCLASS_OTHER,
 /* * */ CCLASS_OTHER,    /* + */ CCLASS_OTHER,   /* , */ CCLASS_OTHER,
 /* - */ CCLASS_OTHER,    /* . */ CCLASS_OTHER,   /* / */ CCLASS_OTHER,
 /* 0 */ CCLASS_DIGIT,    /* 1 */ CCLASS_DIGIT,   /* 2 */ CCLASS_DIGIT,
 /* 3 */ CCLASS_DIGIT,    /* 4 */ CCLASS_DIGIT,   /* 5 */ CCLASS_DIGIT,
 /* 6 */ CCLASS_DIGIT,    /* 7 */ CCLASS_DIGIT,   /* 8 */ CCLASS_DIGIT,
 /* 9 */ CCLASS_DIGIT,    /* : */ CCLASS_OTHER,   /* ; */ CCLASS_OTHER,
 /* < */ CCLASS_OTHER,    /* = */ CCLASS_OTHER,   /* > */ CCLASS_OTHER,
 /* ? */ CCLASS_OTHER,    /* @ */ CCLASS_OTHER,   /* A */ CCLASS_VOWEL,
 /* B */ CCLASS_B,        /* C */ CCLASS_C,       /* D */ CCLASS_D,
 /* E */ CCLASS_VOWEL,    /* F */ CCLASS_B,       /* G */ CCLASS_C,
 /* H */ CCLASS_SILENT,   /* I */ CCLASS_VOWEL,   /* J */ CCLASS_C,
 /* K */ CCLASS_C,        /* L */ CCLASS_L,       /* M */ CCLASS_M,
 /* N */ CCLASS_M,        /* O */ CCLASS_VOWEL,   /* P */ CCLASS_B,
 /* Q */ CCLASS_C,        /* R */ CCLASS_R,       /* S */ CCLASS_C,
 /* T */ CCLASS_D,        /* U */ CCLASS_VOWEL,   /* V */ CCLASS_B,
 /* W */ CCLASS_B,        /* X */ CCLASS_C,       /* Y */ CCLASS_Y,
 /* Z */ CCLASS_C,        /* [ */ CCLASS_OTHER,   /* \ */ CCLASS_OTHER,
 /* ] */ CCLASS_OTHER,    /* ^ */ CCLASS_OTHER,   /* _ */ CCLASS_OTHER,
 /* ` */ CCLASS_OTHER,    /* a */ CCLASS_VOWEL,   /* b */ CCLASS_B,
 /* c */ CCLASS_C,        /* d */ CCLASS_D,       /* e */ CCLASS_VOWEL,
 /* f */ CCLASS_B,        /* g */ CCLASS_C,       /* h */ CCLASS_SILENT,
 /* i */ CCLASS_VOWEL,    /* j */ CCLASS_C,       /* k */ CCLASS_C,
 /* l */ CCLASS_L,        /* m */ CCLASS_M,       /* n */ CCLASS_M,
 /* o */ CCLASS_VOWEL,    /* p */ CCLASS_B,       /* q */ CCLASS_C,
 /* r */ CCLASS_R,        /* s */ CCLASS_C,       /* t */ CCLASS_D,
 /* u */ CCLASS_VOWEL,    /* v */ CCLASS_B,       /* w */ CCLASS_B,
 /* x */ CCLASS_C,        /* y */ CCLASS_Y,       /* z */ CCLASS_C,
 /* { */ CCLASS_OTHER,    /* | */ CCLASS_OTHER,   /* } */ CCLASS_OTHER,
 /* ~ */ CCLASS_OTHER,    /*   */ CCLASS_OTHER,   
};

/*
** Mapping from the character class number (0-13) to a symbol for each
** character class.  Note that __init_class[] can be used to map the class
** symbol back into the class number.
*/
static const unsigned char __class_name[] = ".ABCDHLRMY9 ?";

/*
** Generate a "phonetic hash" from a string of ASCII characters
** in in[0..n_in-1].
**
**   * Map characters by character class as defined above.
**   * Omit double-letters
**   * Omit vowels beside R and L
**   * Omit T when followed by CH
**   * Omit W when followed by R
**   * Omit D when followed by J or G
**   * Omit K in KN or G in GN at the beginning of a word
**
** Space to hold the result is obtained from dbsql_malloc()
**
** Return NULL if memory allocation fails.  
*/
static unsigned char *__phonetic_hash(const unsigned char *in, int n_in){
  unsigned char *out = dbsql_malloc64( (int64_t)n_in + 1 );
  int i;
  int n_out = 0;
  char c_prev = 0x77;
  char c_prev_x = 0x77;
  const unsigned char *classes = __init_class;

  if( out==0 ) return 0;
  if( n_in>2 ){
    switch( in[0] ){
      case 'g': 
      case 'k': {
        if( in[1]=='n' ){ in++; n_in--; }
        break;
      }
    }
  }
  for(i=0; i<n_in; i++){
    unsigned char c = in[i];
    if( i+1<n_in ){
      if( c=='w' && in[i+1]=='r' ) continue;
      if( c=='d' && (in[i+1]=='j' || in[i+1]=='g') ) continue;
      if( i+2<n_in ){
        if( c=='t' && in[i+1]=='c' && in[i+2]=='h' ) continue;
      }
    }
    c = classes[c&0x7f];
    if( c==CCLASS_SPACE ) continue;
    if( c==CCLASS_OTHER && c_prev!=CCLASS_DIGIT ) continue;
    classes = __mid_class;
    if( c==CCLASS_VOWEL && (c_prev_x==CCLASS_R || c_prev_x==CCLASS_L) ){
       continue; /* No vowels beside L or R */ 
    }
    if( (c==CCLASS_R || c==CCLASS_L) && c_prev_x==CCLASS_VOWEL ){
       n_out--;   /* No vowels beside L or R */
    }
    c_prev = c;
    if( c==CCLASS_SILENT ) continue;
    c_prev_x = c;
    c = __class_name[c];
    assert( n_out>=0 );
    if( n_out==0 || c!=out[n_out-1] ) out[n_out++] = c;
  }
  out[n_out] = 0;
  return out;
}

/*
** This is an SQL function wrapper around __phonetic_hash().  See
** the description of __phonetic_hash() for additional information.
*/
static void __phonetic_hash_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *in;
  unsigned char *out;

  in = dbsql_value_text(argv[0]);
  if( in==0 ) return;
  out = __phonetic_hash(in, dbsql_value_bytes(argv[0]));
  if( out==0 ){
    dbsql_result_error_nomem(context);
  }else{
    dbsql_result_text(context, (char*)out, -1, dbsql_free);
  }
}

/*
** Return the character class number for a character given its
** context.
*/
static char __character_class(char c_prev, char c){
  return c_prev==0 ? __init_class[c&0x7f] : __mid_class[c&0x7f];
}

/*
** Return the cost of inserting or deleting character c immediately
** following character c_prev.  If c_prev==0, that means c is the first
** character of the word.
*/
static int __insert_or_delete_cost(char c_prev, char c, char c_next){
  char class_c = __character_class(c_prev, c);
  char class_cprev;

  if( class_c==CCLASS_SILENT ){
    /* Insert or delete "silent" characters such as H or W */
    return 1;
  }
  if( c_prev==c ){
    /* Repeated characters, or miss a repeat */
    return 10;
  }
  if( class_c==CCLASS_VOWEL && (c_prev=='r' || c_next=='r') ){
    return 20;  /* Insert a vowel before or after 'r' */
  }
  class_cprev = __character_class(c_prev, c_prev);
  if( class_c==class_cprev ){
    if( class_c==CCLASS_VOWEL ){
      /* Remove or add a new vowel to a vowel cluster */
      return 15;
    }else{
      /* Remove or add a consonant not in the same class */
      return 50;
    }
  }

  /* any other character insertion or deletion */
  return 100;
}

/*
** Divide the insertion cost by this factor when appending to the
** end of the word.
*/
#define FINAL_INS_COST_DIV  4

/*
** Return the cost of substituting c_to in place of c_from assuming
** the previous character is c_prev.  If c_prev==0 then c_to is the first
** character of the word.
*/
static int __substitute_cost(char c_prev, char c_from, char c_to){
  char class_from, class_to;
  if( c_from==c_to ){
    /* Exact match */
    return 0;
  }
  if( c_from==(c_to^0x20) && ((c_to>='A' && c_to<='Z') || (c_to>='a' && c_to<='z')) ){
    /* differ only in case */
    return 0;
  }
  class_from = __character_class(c_prev, c_from);
  class_to = __character_class(c_prev, c_to);
  if( class_from==class_to ){
    /* Same character class */
    return 40;
  }
  if( class_from>=CCLASS_B && class_from<=CCLASS_Y
      && class_to>=CCLASS_B && class_to<=CCLASS_Y ){
    /* Convert from one consonant to another, but in a different class */
    return 75;
  }
  /* Any other subsitution */
  return 100;
}

/*
** Given two strings a and b which are pure ASCII, return the cost
** of transforming a into b.  If a ends with '*' assume that it is
** a prefix of b and give only minimal penalty for extra characters
** on the end of b.
**
** Smaller numbers mean a closer match.
**
** Negative values indicate an error:
**    -1  One of the inputs is NULL
**    -2  Non-ASCII characters on input
**    -3  Unable to allocate memory
**    -4  Inputs too large
**
** If pn_match is not NULL, then *pn_match is set to the number of bytes
** of b that matched the pattern in a. If a does not end with a '*',
** then this value is always the number of bytes in b (i.e. strlen(b)).
** If a does end in a '*', then it is the number of bytes in the prefix
** of b that was deemed to match a.
*/
static int editdist1(const char *z_a, const char *z_b, int *pn_match){
  unsigned int n_a, n_b;   /* Number of characters in a[] and b[] */
  unsigned int a, b;   /* Loop counters for a[] and b[] */
  char c_a = 0, c_b;       /* Current character of a and b */
  char c_aprev, c_bprev;   /* Previous character of a and b */
  char c_anext, c_bnext;   /* Next character in a and b */
  int d;                 /* North-west cost value */
  int dc = 0;            /* North-west character value */
  int res;               /* Final result */
  int *m;                /* The cost matrix */
  char *cx;              /* Corresponding character values */
  int *to_free = 0;       /* Malloced space */
  int n_match = 0;
  int m_stack[60+15];     /* Stack space to use if not too much is needed */

  /* Early out if either input is NULL */
  if( z_a==0 || z_b==0 ) return -1;

  /* Skip any common prefix */
  while( z_a[0] && z_a[0]==z_b[0] ){ dc = z_a[0]; z_a++; z_b++; n_match++; }
  if( pn_match ) *pn_match = n_match;
  if( z_a[0]==0 && z_b[0]==0 ) return 0;

#if 0
  printf("A=\"%s\" B=\"%s\" dc=%c\n", z_a, z_b, dc?dc:' ');
#endif

  /* Verify input strings and measure their lengths */
  for(n_a=0; z_a[n_a]; n_a++){
    if( z_a[n_a]&0x80 ) return -2;
  }
  if( n_a>=100000 ) return -4;
  for(n_b=0; z_b[n_b]; n_b++){
    if( z_b[n_b]&0x80 ) return -2;
  }
  if( n_b>=100000 ) return -4;

  /* Special processing if either string is empty */
  if( n_a==0 ){
    c_bprev = (char)dc;
    for(b=res=0; (c_b = z_b[b])!=0; b++){
      res += __insert_or_delete_cost(c_bprev, c_b, z_b[b+1])/FINAL_INS_COST_DIV;
      c_bprev = c_b;
    }
    return res;
  }
  if( n_b==0 ){
    c_aprev = (char)dc;
    for(a=res=0; (c_a = z_a[a])!=0; a++){
      res += __insert_or_delete_cost(c_aprev, c_a, z_a[a+1]);
      c_aprev = c_a;
    }
    return res;
  }

  /* A is a prefix of B */
  if( z_a[0]=='*' && z_a[1]==0 ) return 0;

  /* Allocate and initialize the Wagner matrix */
  if( n_b<(sizeof(m_stack)*4)/(sizeof(m_stack[0])*5) ){
    m = m_stack;
  }else{
    m = to_free = dbsql_malloc64( ((int64_t)n_b+1)*5LL*sizeof(m[0])/4 );
    if( m==0 ) return -3;
  }
  cx = (char*)&m[n_b+1];

  /* Compute the Wagner edit distance */
  m[0] = 0;
  cx[0] = (char)dc;
  c_bprev = (char)dc;
  for(b=1; b<=n_b; b++){
    c_bnext = z_b[b];
    c_b = z_b[b-1];
    cx[b] = c_b;
    m[b] = m[b-1] + __insert_or_delete_cost(c_bprev, c_b, c_bnext);
    c_bprev = c_b;
  }
  c_aprev = (char)dc;
  for(a=1; a<=n_a; a++){
    int last_a = (a==n_a);
    c_a = z_a[a-1];
    c_anext = z_a[a];
    if( c_a=='*' && last_a ) break;
    d = m[0];
    dc = cx[0];
    m[0] = d + __insert_or_delete_cost(c_aprev, c_a, c_anext);
    c_bprev = 0;
    for(b=1; b<=n_b; b++){
      int total_cost, ins_cost, del_cost, sub_cost, ncx;
      c_b = z_b[b-1];
      c_bnext = z_b[b];

      /* Cost to insert c_b */
      ins_cost = __insert_or_delete_cost(cx[b-1], c_b, c_bnext);
      if( last_a ) ins_cost /= FINAL_INS_COST_DIV;

      /* Cost to delete c_a */
      del_cost = __insert_or_delete_cost(cx[b], c_a, c_bnext);

      /* Cost to substitute c_a->c_b */
      sub_cost = __substitute_cost(cx[b-1], c_a, c_b);

      /* Best cost */
      total_cost = ins_cost + m[b-1];
      ncx = c_b;
      if( (del_cost + m[b])<total_cost ){
        total_cost = del_cost + m[b];
        ncx = c_a;
      }
      if( (sub_cost + d)<total_cost ){
        total_cost = sub_cost + d;
      }

#if 0
      printf("%d,%d d=%4d u=%4d r=%4d dc=%c cA=%c cB=%c"
             " ins=%4d del=%4d sub=%4d t=%4d ncx=%c\n",
             a, b, d, m[b], m[b-1], dc?dc:' ', c_a, c_b,
             ins_cost, del_cost, sub_cost, total_cost, ncx?ncx:' ');
#endif

      /* Update the matrix */
      d = m[b];
      dc = cx[b];
      m[b] = total_cost;
      cx[b] = (char)ncx;
      c_bprev = c_b;
    }
    c_aprev = c_a;
  }

  /* Free the wagner matrix and return the result */
  if( c_a=='*' ){
    res = m[1];
    for(b=1; b<=n_b; b++){
      if( m[b]<res ){
        res = m[b];
        if( pn_match ) *pn_match = b+n_match;
      }
    }
  }else{
    res = m[n_b];
    /* In the current implementation, pn_match is always NULL if a does
    ** not end in "*" */
    assert( pn_match==0 );
  }
  dbsql_free(to_free);
  return res;
}

/*
** Function:    editdist(A,B)
**
** Return the cost of transforming string A into string B.  Both strings
** must be pure ASCII text.  If A ends with '*' then it is assumed to be
** a prefix of B and extra characters on the end of B have minimal additional
** cost.
*/
static void __editdist_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int res = editdist1(
                    (const char*)dbsql_value_text(argv[0]),
                    (const char*)dbsql_value_text(argv[1]),
                    0);
  if( res<0 ){
    if( res==(-3) ){
      dbsql_result_error_nomem(context);
    }else if( res==(-4) ){
      dbsql_result_error_toobig(context);
    }else if( res==(-2) ){
      dbsql_result_error(context, "non-ASCII input to editdist()", -1);
    }else{
      dbsql_result_error(context, "NULL input to editdist()", -1);
    }
  }else{ 
    dbsql_result_int(context, res);
  }
}

/* End of the fixed-cost edit distance implementation
******************************************************************************
*****************************************************************************
** Begin: Configurable cost unicode edit distance routines
*/
/* Forward declaration of structures */
typedef struct edit_dist3_cost_t edit_dist3_cost_t;
typedef struct edit_dist3_config_t edit_dist3_config_t;
typedef struct edit_dist3_point_t edit_dist3_point_t;
typedef struct edit_dist3_from_t edit_dist3_from_t;
typedef struct edit_dist3_from_string_t edit_dist3_from_string_t;
typedef struct edit_dist3_to_t edit_dist3_to_t;
typedef struct edit_dist3_to_string_t edit_dist3_to_string_t;
typedef struct edit_dist3_lang_t edit_dist3_lang_t;


/*
** An entry in the edit cost table
*/
struct edit_dist3_cost_t {
  edit_dist3_cost_t *next;     /* Next cost element */
  u_int8_t n_from;                 /* Number of bytes in froms */
  u_int8_t n_to;                   /* Number of bytes in tos */
  u_int16_t i_cost;                /* Cost of this transformation */
  char a[4]    ;            /* FROM string followed by TO string */
  /* Additional TO and FROM string bytes appended as necessary */
};

/*
** Edit costs for a particular language ID 
*/
struct edit_dist3_lang_t {
  int i_lang;             /* Language ID */
  int i_ins_cost;          /* Default insertion cost */
  int i_del_cost;          /* Default deletion cost */
  int i_sub_cost;          /* Default substitution cost */
  edit_dist3_cost_t *cost;  /* Costs */
};


/*
** The default edit_dist3_lang_t object, with default costs.
*/
static const edit_dist3_lang_t __edit_dist3_lang = { 0, 100, 100, 150, 0 };

/*
** Complete configuration
*/
struct edit_dist3_config_t {
  int n_lang;             /* Number of language IDs.  Size of a[] */
  edit_dist3_lang_t *a;      /* One for each distinct language ID */
};

/*
** Extra information about each character in the FROM string.
*/
struct edit_dist3_from_t {
  int n_subst;              /* Number of substitution cost entries */
  int n_del;                /* Number of deletion cost entries */
  int n_byte;               /* Number of bytes in this character */
  edit_dist3_cost_t **substs; /* Array of substitution costs for this element */
  edit_dist3_cost_t **dels;   /* Array of deletion cost entries */
};

/*
** A precompiled FROM string.
*
** In the common case we expect the FROM string to be reused multiple times.
** In other words, the common case will be to measure the edit distance
** from a single origin string to multiple target strings.
*/
struct edit_dist3_from_string_t {
  char *z;                 /* The complete text of the FROM string */
  int n;                   /* Number of characters in the FROM string */
  int isPrefix;            /* True if ends with '*' character */
  edit_dist3_from_t *a;        /* Extra info about each char of the FROM string */
};

/*
** Extra information about each character in the TO string.
*/
struct edit_dist3_to_t {
  int n_ins;                /* Number of insertion cost entries */
  int n_byte;               /* Number of bytes in this character */
  edit_dist3_cost_t **inses;   /* Array of deletion cost entries */
};

/*
** A precompiled FROM string
*/
struct edit_dist3_to_string_t {
  char *z;                 /* The complete text of the TO string */
  int n;                   /* Number of characters in the TO string */
  edit_dist3_to_t *a;          /* Extra info about each char of the TO string */
};

/*
** Clear or delete an instance of the object that records all edit-distance
** weights.
*/
static void __edit_dist3_config_clear(edit_dist3_config_t *p){
  int i;
  if( p==0 ) return;
  for(i=0; i<p->n_lang; i++){
    edit_dist3_cost_t *cost, *next;
    cost = p->a[i].cost;
    while( cost ){
      next = cost->next;
      dbsql_free(cost);
      cost = next;
    }
  }
  dbsql_free(p->a);
  memset(p, 0, sizeof(*p));
}
static void __edit_dist3_config_delete(void *in){
  edit_dist3_config_t *p = (edit_dist3_config_t*)in;
  __edit_dist3_config_clear(p);
  dbsql_free(p);
}

/* Compare the FROM values of two edit_dist3_cost_t objects, for sorting.
** Return negative, zero, or positive if the A is less than, equal to,
** or greater than B.
*/
static int __edit_dist3_cost_compare(edit_dist3_cost_t *a, edit_dist3_cost_t *b){
  int n = a->n_from;
  int rc;
  if( n>b->n_from ) n = b->n_from;
  rc = strncmp(a->a, b->a, n);
  if( rc==0 ) rc = a->n_from - b->n_from;
  return rc;
}

/*
** Merge together two sorted lists of edit_dist3_cost_t objects, in order
** of increasing FROM.
*/
static edit_dist3_cost_t *__edit_dist3_cost_merge(
  edit_dist3_cost_t *a,
  edit_dist3_cost_t *b
){
  edit_dist3_cost_t *head = 0;
  edit_dist3_cost_t **tail = &head;
  edit_dist3_cost_t *p;
  while( a && b ){
    if( __edit_dist3_cost_compare(a,b)<=0 ){
      p = a;
      a = a->next;
    }else{
      p = b;
      b = b->next;
    }
    *tail = p;
    tail =  &p->next;
  }
  if( a ){
    *tail = a;
  }else{
    *tail = b;
  }
  return head;
}

/*
** Sort a list of edit_dist3_cost_t objects into order of increasing FROM
*/
static edit_dist3_cost_t *__edit_dist3_cost_sort(edit_dist3_cost_t *list){
  edit_dist3_cost_t *ap[60], *p;
  int i;
  int mx = 0;
  ap[0] = 0;
  ap[1] = 0;
  while( list ){
    p = list;
    list = p->next;
    p->next = 0;
    for(i=0; ap[i]; i++){
      p = __edit_dist3_cost_merge(ap[i],p);
      ap[i] = 0;
    }
    ap[i] = p;
    if( i>mx ){
      mx = i;
      ap[i+1] = 0;
    }
  }
  p = 0;
  for(i=0; i<=mx; i++){
    if( ap[i] ) p = __edit_dist3_cost_merge(p,ap[i]);
  }
  return p;
}

/*
** Load all edit-distance weights from a table.
*/
static int __edit_dist3_config_load(
  edit_dist3_config_t *p,      /* The edit distance configuration to load */
  DBSQL *db,            /* Load from this database */
  const char *table      /* Name of the table from which to load */
){
  dbsql_stmt_t *stmt;
  int rc, rc2;
  char *sql;
  int i_lang_prev = -9999;
  edit_dist3_lang_t *lang = 0;

  sql = dbsql_mprintf("SELECT iLang, cFrom, cTo, iCost"
                         " FROM \"%w\" WHERE iLang>=0 ORDER BY iLang", table);
  if( sql==0 ) return DBSQL_NOMEM;
  rc = dbsql_prepare(db, sql, -1, &stmt, 0);
  dbsql_free(sql);
  if( rc ) return rc;
  __edit_dist3_config_clear(p);
  while( dbsql_step(stmt)==DBSQL_ROW ){
    int i_lang = dbsql_column_int(stmt, 0);
    const char *from = (const char*)dbsql_column_text(stmt, 1);
    int n_from = from ? dbsql_column_bytes(stmt, 1) : 0;
    const char *to = (const char*)dbsql_column_text(stmt, 2);
    int n_to = to ? dbsql_column_bytes(stmt, 2) : 0;
    int i_cost = dbsql_column_int(stmt, 3);

    assert( from!=0 || n_from==0 );
    assert( to!=0 || n_to==0 );
    if( n_from>100 || n_to>100 ) continue;
    if( i_cost<0 ) continue;
    if( i_cost>=10000 ) continue;  /* Costs above 10K are considered infinite */
    if( lang==0 || i_lang!=i_lang_prev ){
      edit_dist3_lang_t *p_new;
      p_new = dbsql_realloc64(p->a, ((int64_t)p->n_lang+1)*sizeof(p->a[0]));
      if( p_new==0 ){ rc = DBSQL_NOMEM; break; }
      p->a = p_new;
      lang = &p->a[p->n_lang];
      p->n_lang++;
      lang->i_lang = i_lang;
      lang->i_ins_cost = 100;
      lang->i_del_cost = 100;
      lang->i_sub_cost = 150;
      lang->cost = 0;
      i_lang_prev = i_lang;
    }
    if( n_from==1 && from[0]=='?' && n_to==0 ){
      lang->i_del_cost = i_cost;
    }else if( n_from==0 && n_to==1 && to[0]=='?' ){
      lang->i_ins_cost = i_cost;
    }else if( n_from==1 && n_to==1 && from[0]=='?' && to[0]=='?' ){
      lang->i_sub_cost = i_cost;
    }else{
      edit_dist3_cost_t *cost;
      int n_extra = n_from + n_to - 4;
      if( n_extra<0 ) n_extra = 0;
      cost = dbsql_malloc64( sizeof(*cost) + n_extra );
      if( cost==0 ){ rc = DBSQL_NOMEM; break; }
      cost->n_from = (u_int8_t)n_from;
      cost->n_to = (u_int8_t)n_to;
      cost->i_cost = (u_int16_t)i_cost;
      memcpy(cost->a, from, n_from);
      memcpy(cost->a + n_from, to, n_to);
      cost->next = lang->cost;
      lang->cost = cost; 
    }
  }
  rc2 = dbsql_finalize(stmt);
  if( rc==DBSQL_OK ) rc = rc2;
  if( rc==DBSQL_OK ){
    int i_lang;
    for(i_lang=0; i_lang<p->n_lang; i_lang++){
      p->a[i_lang].cost = __edit_dist3_cost_sort(p->a[i_lang].cost);
    }
  }
  return rc;
}

/*
** Return the length (in bytes) of a utf-8 character.  Or return a maximum
** of N.
*/
static int utf8Len(unsigned char c, int N){
  int len = 1;
  if( c>0x7f ){
    if( (c&0xe0)==0xc0 ){
      len = 2;
    }else if( (c&0xf0)==0xe0 ){
      len = 3;
    }else{
      len = 4;
    }
  }
  if( len>N ) len = N;
  return len;
}

/*
** Return TRUE (non-zero) if the To side of the given cost matches
** the given string.
*/
static int __match_to(edit_dist3_cost_t *p, const char *z, int n){
  assert( n>0 );
  if( p->a[p->n_from]!=z[0] ) return 0;
  if( p->n_to>n ) return 0;
  if( strncmp(p->a+p->n_from, z, p->n_to)!=0 ) return 0;
  return 1;
}

/*
** Return TRUE (non-zero) if the From side of the given cost matches
** the given string.
*/
static int __match_from(edit_dist3_cost_t *p, const char *z, int n){
  assert( p->n_from<=n );
  if( p->n_from ){
    if( p->a[0]!=z[0] ) return 0;
    if( strncmp(p->a, z, p->n_from)!=0 ) return 0;
  }
  return 1;
}

/*
** Return TRUE (non-zero) of the next FROM character and the next TO
** character are the same.
*/
static int __match_from_to(
  edit_dist3_from_string_t *str,  /* Left hand string */
  int n1,                     /* index_t of comparison character on the left */
  const char *z2,             /* Right-handl comparison character */
  int n2                      /* Bytes remaining in z2[] */
){
  int b1 = str->a[n1].n_byte;
  if( b1>n2 ) return 0;
  assert( b1>0 );
  if( str->z[n1]!=z2[0] ) return 0;
  if( strncmp(str->z+n1, z2, b1)!=0 ) return 0;
  return 1;
}

/*
** Delete an edit_dist3_from_string_t objecct
*/
static void __edit_dist3_from_string_delete(edit_dist3_from_string_t *p){
  int i;
  if( p ){
    for(i=0; i<p->n; i++){
      dbsql_free(p->a[i].dels);
      dbsql_free(p->a[i].substs);
    }
    dbsql_free(p);
  }
}

/*
** Create a edit_dist3_from_string_t object.
*/
static edit_dist3_from_string_t *__edit_dist3_from_string_new(
  const edit_dist3_lang_t *lang,
  const char *z,
  int n
){
  edit_dist3_from_string_t *str;
  edit_dist3_cost_t *p;
  int i;

  if( z==0 ) return 0;
  if( n<0 ) n = (int)strlen(z);
  str = dbsql_malloc64( sizeof(*str) + sizeof(str->a[0])*n + (int64_t)n + 1 );
  if( str==0 ) return 0;
  str->a = (edit_dist3_from_t*)&str[1];
  memset(str->a, 0, sizeof(str->a[0])*n);
  str->n = n;
  str->z = (char*)&str->a[n];
  memcpy(str->z, z, n+1);
  if( n && z[n-1]=='*' ){
    str->isPrefix = 1;
    n--;
    str->n--;
    str->z[n] = 0;
  }else{
    str->isPrefix = 0;
  }

  for(i=0; i<n; i++){
    edit_dist3_from_t *from = &str->a[i];
    memset(from, 0, sizeof(*from));
    from->n_byte = utf8Len((unsigned char)z[i], n-i);
    for(p=lang->cost; p; p=p->next){
      edit_dist3_cost_t **news;
      if( i+p->n_from>n ) continue;
      if( __match_from(p, z+i, n-i)==0 ) continue;
      if( p->n_to==0 ){
        news = dbsql_realloc64(from->dels,
                                sizeof(*news)*((int64_t)from->n_del+1));
        if( news==0 ) break;
        from->dels = news;
        news[from->n_del++] = p;
      }else{
        news = dbsql_realloc64(from->substs,
                                sizeof(*news)*((int64_t)from->n_subst+1));
        if( news==0 ) break;
        from->substs = news;
        news[from->n_subst++] = p;
      }
    }
    if( p ){
      __edit_dist3_from_string_delete(str);
      str = 0;
      break;
    }
  }
  return str;
}

/*
** Update entry m[i] such that it is the minimum of its current value
** and m[j]+i_cost.
*/
static void __update_cost(
  unsigned int *m,
  int i,
  int j,
  int i_cost
){
  unsigned int b;
  assert( i_cost>=0 );
  assert( i_cost<10000 );
  b = m[j] + i_cost;
  if( b<m[i] ) m[i] = b;
}

/*
** How much stack space (int bytes) to use for Wagner matrix in 
** __edit_dist3_core().  If more space than this is required, the entire
** matrix is taken from the heap.  To reduce the load on the memory
** allocator, make this value as large as practical for the
** architecture in use.
*/
#ifndef DBSQL_SPELLFIX_STACKALLOC_SZ
# define DBSQL_SPELLFIX_STACKALLOC_SZ  (1024)
#endif

/* Compute the edit distance between two strings.
**
** If an error occurs, return a negative number which is the error code.
**
** If pn_match is not NULL, then *pn_match is set to the number of characters
** (not bytes) in z2 that matched the search pattern in *from. If from does
** not contain the pattern for a prefix-search, then this is always the number
** of characters in z2. If from does contain a prefix search pattern, then
** it is the number of characters in the prefix of z2 that was deemed to 
** match from.
*/
static int __edit_dist3_core(
  edit_dist3_from_string_t *from,  /* The FROM string */
  const char *z2,              /* The TO string */
  int n2,                      /* Length of the TO string */
  const edit_dist3_lang_t *lang,  /* Edit weights for a particular language ID */
  int *pn_match                 /* OUT: Characters in matched prefix */
){
  int k, n;
  int i1, b1;
  int i2, b2;
  edit_dist3_from_string_t f = *from;
  edit_dist3_to_t *a2;
  unsigned int *m;
  unsigned int *to_free;
  int sz_row;
  edit_dist3_cost_t *p;
  int res;
  u_int64_t n_byte;
  unsigned int stack_space[DBSQL_SPELLFIX_STACKALLOC_SZ/sizeof(unsigned int)];

  /* allocate the Wagner matrix and the tos[] array for the TO string */
  if( n2>10000 ) return -2;
  if( f.n>10000 ) return -2;
  n = (f.n+1)*(n2+1);
  n = (n+1)&~1;
  n_byte = n*sizeof(m[0]) + sizeof(a2[0])*n2;
  if( n_byte<=sizeof(stack_space) ){
    m = stack_space;
    to_free = 0;
  }else{
    m = to_free = dbsql_malloc64( n_byte );
    if( m==0 ) return -1;            /* Out of memory */
  }
  a2 = (edit_dist3_to_t*)&m[n];
  memset(a2, 0, sizeof(a2[0])*n2);

  /* Fill in the a1[] matrix for all characters of the TO string */
  for(i2=0; i2<n2; i2++){
    a2[i2].n_byte = utf8Len((unsigned char)z2[i2], n2-i2);
    for(p=lang->cost; p; p=p->next){
      edit_dist3_cost_t **news;
      if( p->n_from>0 ) break;
      if( i2+p->n_to>n2 ) continue;
      if( p->a[0]>z2[i2] ) break;
      if( __match_to(p, z2+i2, n2-i2)==0 ) continue;
      a2[i2].n_ins++;
      news = dbsql_realloc64(a2[i2].inses, sizeof(*news)*a2[i2].n_ins);
      if( news==0 ){
        res = -1;  /* Out of memory */
        goto edit_dist3_abort;
      }
      a2[i2].inses = news;
      a2[i2].inses[a2[i2].n_ins-1] = p;
    }
  }

  /* Prepare to compute the minimum edit distance */
  sz_row = f.n+1;
  memset(m, 0x01, (n2+1)*sz_row*sizeof(m[0]));
  m[0] = 0;

  /* First fill in the top-row of the matrix with FROM deletion costs */
  for(i1=0; i1<f.n; i1 += b1){
    b1 = f.a[i1].n_byte;
    __update_cost(m, i1+b1, i1, lang->i_del_cost);
    for(k=0; k<f.a[i1].n_del; k++){
      p = f.a[i1].dels[k];
      __update_cost(m, i1+p->n_from, i1, p->i_cost);
    }
  }

  /* Fill in all subsequent rows, top-to-bottom, left-to-right */
  for(i2=0; i2<n2; i2 += b2){
    int rx;      /* Starting index for current row */
    int rxp;     /* Starting index for previous row */
    b2 = a2[i2].n_byte;
    rx = sz_row*(i2+b2);
    rxp = sz_row*i2;
    __update_cost(m, rx, rxp, lang->i_ins_cost);
    for(k=0; k<a2[i2].n_ins; k++){
      p = a2[i2].inses[k];
      __update_cost(m, sz_row*(i2+p->n_to), rxp, p->i_cost);
    }
    for(i1=0; i1<f.n; i1+=b1){
      int cx;    /* index_t of current cell */
      int cxp;   /* index_t of cell immediately to the left */
      int cxd;   /* index_t of cell to the left and one row above */
      int cxu;   /* index_t of cell immediately above */
      b1 = f.a[i1].n_byte;
      cxp = rx + i1;
      cx = cxp + b1;
      cxd = rxp + i1;
      cxu = cxd + b1;
      __update_cost(m, cx, cxp, lang->i_del_cost);
      for(k=0; k<f.a[i1].n_del; k++){
        p = f.a[i1].dels[k];
        __update_cost(m, cxp+p->n_from, cxp, p->i_cost);
      }
      __update_cost(m, cx, cxu, lang->i_ins_cost);
      if( __match_from_to(&f, i1, z2+i2, n2-i2) ){
        __update_cost(m, cx, cxd, 0);
      }
      __update_cost(m, cx, cxd, lang->i_sub_cost);
      for(k=0; k<f.a[i1].n_subst; k++){
        p = f.a[i1].substs[k];
        if( __match_to(p, z2+i2, n2-i2) ){
          __update_cost(m, cxd+p->n_from+sz_row*p->n_to, cxd, p->i_cost);
        }
      }
    }
  }

#if 0  /* Enable for debugging */
  printf("         ^");
  for(i1=0; i1<f.n; i1++) printf(" %c-%2x", f.z[i1], f.z[i1]&0xff);
  printf("\n   ^:");
  for(i1=0; i1<sz_row; i1++){
    int v = m[i1];
    if( v>9999 ) printf(" ****");
    else         printf(" %4d", v);
  }
  printf("\n");
  for(i2=0; i2<n2; i2++){
    printf("%c-%02x:", z2[i2], z2[i2]&0xff);
    for(i1=0; i1<sz_row; i1++){
      int v = m[(i2+1)*sz_row+i1];
      if( v>9999 ) printf(" ****");
      else         printf(" %4d", v);
    }
    printf("\n");
  }
#endif

  /* Free memory allocations and return the result */
  res = (int)m[sz_row*(n2+1)-1];
  n = n2;
  if( f.isPrefix ){
    for(i2=1; i2<=n2; i2++){
      int b = m[sz_row*i2-1];
      if( b<=res ){ 
        res = b;
        n = i2 - 1;
      }
    }
  }
  if( pn_match ){
    int n_extra = 0;
    for(k=0; k<n; k++){
      if( (z2[k] & 0xc0)==0x80 ) n_extra++;
    }
    *pn_match = n - n_extra;
  }

edit_dist3_abort:
  for(i2=0; i2<n2; i2++) dbsql_free(a2[i2].inses);
  dbsql_free(to_free);
  return res;
}

/*
** Get an appropriate edit_dist3_lang_t object.
*/
static const edit_dist3_lang_t *__edit_dist3_find_lang(
  edit_dist3_config_t *config,
  int i_lang
){
  int i;
  for(i=0; i<config->n_lang; i++){
    if( config->a[i].i_lang==i_lang ) return &config->a[i];
  }
  return &__edit_dist3_lang;
}

/*
** Function:    editdist3(A,B,i_lang)
**              editdist3(tablename)
**
** Return the cost of transforming string A into string B using edit
** weights for i_lang.
**
** The second form loads edit weights into memory from a table.
*/
static void __edit_dist3_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  edit_dist3_config_t *config = (edit_dist3_config_t*)dbsql_user_data(context);
  DBSQL *db = dbsql_context_db_handle(context);
  int rc;
  if( argc==1 ){
    const char *table = (const char*)dbsql_value_text(argv[0]);
    rc = __edit_dist3_config_load(config, db, table);
    if( rc ) dbsql_result_error_code(context, rc);
  }else{
    const char *a = (const char*)dbsql_value_text(argv[0]);
    const char *b = (const char*)dbsql_value_text(argv[1]);
    int n_a = dbsql_value_bytes(argv[0]);
    int n_b = dbsql_value_bytes(argv[1]);
    int i_lang = argc==3 ? dbsql_value_int(argv[2]) : 0;
    const edit_dist3_lang_t *lang = __edit_dist3_find_lang(config, i_lang);
    edit_dist3_from_string_t *from;
    int dist;

    from = __edit_dist3_from_string_new(lang, a, n_a);
    if( from==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
    dist = __edit_dist3_core(from, b, n_b, lang, 0);
    __edit_dist3_from_string_delete(from);
    if( dist==(-1) ){
      dbsql_result_error_nomem(context);
    }else if( dist==(-2) ){
      dbsql_result_error_toobig(context);
    }else{
      dbsql_result_int(context, dist);
    }
  } 
}

/*
** Register the edit_dist3 function with dbsql
*/
static int __edit_dist3_install(DBSQL *db){
  int rc;
  edit_dist3_config_t *config = dbsql_malloc64( sizeof(*config) );
  if( config==0 ) return DBSQL_NOMEM;
  memset(config, 0, sizeof(*config));
  rc = dbsql_create_function_v2(db, "editdist3",
              2, DBSQL_UTF8|DBSQL_DETERMINISTIC, config,
              __edit_dist3_sql_func, 0, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function_v2(db, "editdist3",
                3, DBSQL_UTF8|DBSQL_DETERMINISTIC, config,
                __edit_dist3_sql_func, 0, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function_v2(db, "editdist3",
                1, DBSQL_UTF8|DBSQL_DETERMINISTIC, config,
                __edit_dist3_sql_func, 0, 0, __edit_dist3_config_delete);
  }else{
    dbsql_free(config);
  }
  return rc;
}
/* End configurable cost unicode edit distance routines
******************************************************************************
******************************************************************************
** Begin transliterate unicode-to-ascii implementation
*/

#if !DBSQL_AMALGAMATION
/*
** This lookup table is used to help decode the first byte of
** a multi-byte UTF8 character.
*/
static const unsigned char __utf8_trans1[] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00,
};
#endif

/*
** Return the value of the first UTF-8 character in the string.
*/
static int utf8Read(const unsigned char *z, int n, int *size){
  int c, i;

  /* All callers to this routine (in the current implementation)
  ** always have n>0. */
  if( n==0 ){
    c = i = 0;
  }else{
    c = z[0];
    i = 1;
    if( c>=0xc0 ){
      c = __utf8_trans1[c-0xc0];
      while( i<n && (z[i] & 0xc0)==0x80 ){
        c = (c<<6) + (0x3f & z[i++]);
      }
    }
  }
  *size = i;
  return c;
}

/*
** Return the number of characters in the utf-8 string in the n_in byte
** buffer pointed to by in.
*/
static int utf8Charlen(const char *in, int n_in){
  int i;
  int n_char = 0;
  for(i=0; i<n_in; n_char++){
    int sz;
    utf8Read((const unsigned char *)&in[i], n_in-i, &sz);
    i += sz;
  }
  return n_char;
}

typedef struct transliteration_t transliteration_t;
struct transliteration_t {
 unsigned short int c_from;
 unsigned char c_to0, c_to1, c_to2, c_to3;
#ifdef DBSQL_SPELLFIX_5BYTE_MAPPINGS
 unsigned char c_to4;
#endif
};

/*
** table_t of translations from unicode characters into ASCII.
*/
static const transliteration_t translit[] = {
  { 0x00A0,  0x20, 0x00, 0x00, 0x00 },  /*   to   */
  { 0x00B5,  0x75, 0x00, 0x00, 0x00 },  /* µ to u */
  { 0x00C0,  0x41, 0x00, 0x00, 0x00 },  /* À to A */
  { 0x00C1,  0x41, 0x00, 0x00, 0x00 },  /* Á to A */
  { 0x00C2,  0x41, 0x00, 0x00, 0x00 },  /* Â to A */
  { 0x00C3,  0x41, 0x00, 0x00, 0x00 },  /* Ã to A */
  { 0x00C4,  0x41, 0x65, 0x00, 0x00 },  /* Ä to Ae */
  { 0x00C5,  0x41, 0x61, 0x00, 0x00 },  /* Å to Aa */
  { 0x00C6,  0x41, 0x45, 0x00, 0x00 },  /* Æ to AE */
  { 0x00C7,  0x43, 0x00, 0x00, 0x00 },  /* Ç to C */
  { 0x00C8,  0x45, 0x00, 0x00, 0x00 },  /* È to E */
  { 0x00C9,  0x45, 0x00, 0x00, 0x00 },  /* É to E */
  { 0x00CA,  0x45, 0x00, 0x00, 0x00 },  /* Ê to E */
  { 0x00CB,  0x45, 0x00, 0x00, 0x00 },  /* Ë to E */
  { 0x00CC,  0x49, 0x00, 0x00, 0x00 },  /* Ì to I */
  { 0x00CD,  0x49, 0x00, 0x00, 0x00 },  /* Í to I */
  { 0x00CE,  0x49, 0x00, 0x00, 0x00 },  /* Î to I */
  { 0x00CF,  0x49, 0x00, 0x00, 0x00 },  /* Ï to I */
  { 0x00D0,  0x44, 0x00, 0x00, 0x00 },  /* Ð to D */
  { 0x00D1,  0x4E, 0x00, 0x00, 0x00 },  /* Ñ to N */
  { 0x00D2,  0x4F, 0x00, 0x00, 0x00 },  /* Ò to O */
  { 0x00D3,  0x4F, 0x00, 0x00, 0x00 },  /* Ó to O */
  { 0x00D4,  0x4F, 0x00, 0x00, 0x00 },  /* Ô to O */
  { 0x00D5,  0x4F, 0x00, 0x00, 0x00 },  /* Õ to O */
  { 0x00D6,  0x4F, 0x65, 0x00, 0x00 },  /* Ö to Oe */
  { 0x00D7,  0x78, 0x00, 0x00, 0x00 },  /* × to x */
  { 0x00D8,  0x4F, 0x00, 0x00, 0x00 },  /* Ø to O */
  { 0x00D9,  0x55, 0x00, 0x00, 0x00 },  /* Ù to U */
  { 0x00DA,  0x55, 0x00, 0x00, 0x00 },  /* Ú to U */
  { 0x00DB,  0x55, 0x00, 0x00, 0x00 },  /* Û to U */
  { 0x00DC,  0x55, 0x65, 0x00, 0x00 },  /* Ü to Ue */
  { 0x00DD,  0x59, 0x00, 0x00, 0x00 },  /* Ý to Y */
  { 0x00DE,  0x54, 0x68, 0x00, 0x00 },  /* Þ to Th */
  { 0x00DF,  0x73, 0x73, 0x00, 0x00 },  /* ß to ss */
  { 0x00E0,  0x61, 0x00, 0x00, 0x00 },  /* à to a */
  { 0x00E1,  0x61, 0x00, 0x00, 0x00 },  /* á to a */
  { 0x00E2,  0x61, 0x00, 0x00, 0x00 },  /* â to a */
  { 0x00E3,  0x61, 0x00, 0x00, 0x00 },  /* ã to a */
  { 0x00E4,  0x61, 0x65, 0x00, 0x00 },  /* ä to ae */
  { 0x00E5,  0x61, 0x61, 0x00, 0x00 },  /* å to aa */
  { 0x00E6,  0x61, 0x65, 0x00, 0x00 },  /* æ to ae */
  { 0x00E7,  0x63, 0x00, 0x00, 0x00 },  /* ç to c */
  { 0x00E8,  0x65, 0x00, 0x00, 0x00 },  /* è to e */
  { 0x00E9,  0x65, 0x00, 0x00, 0x00 },  /* é to e */
  { 0x00EA,  0x65, 0x00, 0x00, 0x00 },  /* ê to e */
  { 0x00EB,  0x65, 0x00, 0x00, 0x00 },  /* ë to e */
  { 0x00EC,  0x69, 0x00, 0x00, 0x00 },  /* ì to i */
  { 0x00ED,  0x69, 0x00, 0x00, 0x00 },  /* í to i */
  { 0x00EE,  0x69, 0x00, 0x00, 0x00 },  /* î to i */
  { 0x00EF,  0x69, 0x00, 0x00, 0x00 },  /* ï to i */
  { 0x00F0,  0x64, 0x00, 0x00, 0x00 },  /* ð to d */
  { 0x00F1,  0x6E, 0x00, 0x00, 0x00 },  /* ñ to n */
  { 0x00F2,  0x6F, 0x00, 0x00, 0x00 },  /* ò to o */
  { 0x00F3,  0x6F, 0x00, 0x00, 0x00 },  /* ó to o */
  { 0x00F4,  0x6F, 0x00, 0x00, 0x00 },  /* ô to o */
  { 0x00F5,  0x6F, 0x00, 0x00, 0x00 },  /* õ to o */
  { 0x00F6,  0x6F, 0x65, 0x00, 0x00 },  /* ö to oe */
  { 0x00F7,  0x3A, 0x00, 0x00, 0x00 },  /* ÷ to : */
  { 0x00F8,  0x6F, 0x00, 0x00, 0x00 },  /* ø to o */
  { 0x00F9,  0x75, 0x00, 0x00, 0x00 },  /* ù to u */
  { 0x00FA,  0x75, 0x00, 0x00, 0x00 },  /* ú to u */
  { 0x00FB,  0x75, 0x00, 0x00, 0x00 },  /* û to u */
  { 0x00FC,  0x75, 0x65, 0x00, 0x00 },  /* ü to ue */
  { 0x00FD,  0x79, 0x00, 0x00, 0x00 },  /* ý to y */
  { 0x00FE,  0x74, 0x68, 0x00, 0x00 },  /* þ to th */
  { 0x00FF,  0x79, 0x00, 0x00, 0x00 },  /* ÿ to y */
  { 0x0100,  0x41, 0x00, 0x00, 0x00 },  /* Ā to A */
  { 0x0101,  0x61, 0x00, 0x00, 0x00 },  /* ā to a */
  { 0x0102,  0x41, 0x00, 0x00, 0x00 },  /* Ă to A */
  { 0x0103,  0x61, 0x00, 0x00, 0x00 },  /* ă to a */
  { 0x0104,  0x41, 0x00, 0x00, 0x00 },  /* Ą to A */
  { 0x0105,  0x61, 0x00, 0x00, 0x00 },  /* ą to a */
  { 0x0106,  0x43, 0x00, 0x00, 0x00 },  /* Ć to C */
  { 0x0107,  0x63, 0x00, 0x00, 0x00 },  /* ć to c */
  { 0x0108,  0x43, 0x68, 0x00, 0x00 },  /* Ĉ to Ch */
  { 0x0109,  0x63, 0x68, 0x00, 0x00 },  /* ĉ to ch */
  { 0x010A,  0x43, 0x00, 0x00, 0x00 },  /* Ċ to C */
  { 0x010B,  0x63, 0x00, 0x00, 0x00 },  /* ċ to c */
  { 0x010C,  0x43, 0x00, 0x00, 0x00 },  /* Č to C */
  { 0x010D,  0x63, 0x00, 0x00, 0x00 },  /* č to c */
  { 0x010E,  0x44, 0x00, 0x00, 0x00 },  /* Ď to D */
  { 0x010F,  0x64, 0x00, 0x00, 0x00 },  /* ď to d */
  { 0x0110,  0x44, 0x00, 0x00, 0x00 },  /* Đ to D */
  { 0x0111,  0x64, 0x00, 0x00, 0x00 },  /* đ to d */
  { 0x0112,  0x45, 0x00, 0x00, 0x00 },  /* Ē to E */
  { 0x0113,  0x65, 0x00, 0x00, 0x00 },  /* ē to e */
  { 0x0114,  0x45, 0x00, 0x00, 0x00 },  /* Ĕ to E */
  { 0x0115,  0x65, 0x00, 0x00, 0x00 },  /* ĕ to e */
  { 0x0116,  0x45, 0x00, 0x00, 0x00 },  /* Ė to E */
  { 0x0117,  0x65, 0x00, 0x00, 0x00 },  /* ė to e */
  { 0x0118,  0x45, 0x00, 0x00, 0x00 },  /* Ę to E */
  { 0x0119,  0x65, 0x00, 0x00, 0x00 },  /* ę to e */
  { 0x011A,  0x45, 0x00, 0x00, 0x00 },  /* Ě to E */
  { 0x011B,  0x65, 0x00, 0x00, 0x00 },  /* ě to e */
  { 0x011C,  0x47, 0x68, 0x00, 0x00 },  /* Ĝ to Gh */
  { 0x011D,  0x67, 0x68, 0x00, 0x00 },  /* ĝ to gh */
  { 0x011E,  0x47, 0x00, 0x00, 0x00 },  /* Ğ to G */
  { 0x011F,  0x67, 0x00, 0x00, 0x00 },  /* ğ to g */
  { 0x0120,  0x47, 0x00, 0x00, 0x00 },  /* Ġ to G */
  { 0x0121,  0x67, 0x00, 0x00, 0x00 },  /* ġ to g */
  { 0x0122,  0x47, 0x00, 0x00, 0x00 },  /* Ģ to G */
  { 0x0123,  0x67, 0x00, 0x00, 0x00 },  /* ģ to g */
  { 0x0124,  0x48, 0x68, 0x00, 0x00 },  /* Ĥ to Hh */
  { 0x0125,  0x68, 0x68, 0x00, 0x00 },  /* ĥ to hh */
  { 0x0126,  0x48, 0x00, 0x00, 0x00 },  /* Ħ to H */
  { 0x0127,  0x68, 0x00, 0x00, 0x00 },  /* ħ to h */
  { 0x0128,  0x49, 0x00, 0x00, 0x00 },  /* Ĩ to I */
  { 0x0129,  0x69, 0x00, 0x00, 0x00 },  /* ĩ to i */
  { 0x012A,  0x49, 0x00, 0x00, 0x00 },  /* Ī to I */
  { 0x012B,  0x69, 0x00, 0x00, 0x00 },  /* ī to i */
  { 0x012C,  0x49, 0x00, 0x00, 0x00 },  /* Ĭ to I */
  { 0x012D,  0x69, 0x00, 0x00, 0x00 },  /* ĭ to i */
  { 0x012E,  0x49, 0x00, 0x00, 0x00 },  /* Į to I */
  { 0x012F,  0x69, 0x00, 0x00, 0x00 },  /* į to i */
  { 0x0130,  0x49, 0x00, 0x00, 0x00 },  /* İ to I */
  { 0x0131,  0x69, 0x00, 0x00, 0x00 },  /* ı to i */
  { 0x0132,  0x49, 0x4A, 0x00, 0x00 },  /* Ĳ to IJ */
  { 0x0133,  0x69, 0x6A, 0x00, 0x00 },  /* ĳ to ij */
  { 0x0134,  0x4A, 0x68, 0x00, 0x00 },  /* Ĵ to Jh */
  { 0x0135,  0x6A, 0x68, 0x00, 0x00 },  /* ĵ to jh */
  { 0x0136,  0x4B, 0x00, 0x00, 0x00 },  /* Ķ to K */
  { 0x0137,  0x6B, 0x00, 0x00, 0x00 },  /* ķ to k */
  { 0x0138,  0x6B, 0x00, 0x00, 0x00 },  /* ĸ to k */
  { 0x0139,  0x4C, 0x00, 0x00, 0x00 },  /* Ĺ to L */
  { 0x013A,  0x6C, 0x00, 0x00, 0x00 },  /* ĺ to l */
  { 0x013B,  0x4C, 0x00, 0x00, 0x00 },  /* Ļ to L */
  { 0x013C,  0x6C, 0x00, 0x00, 0x00 },  /* ļ to l */
  { 0x013D,  0x4C, 0x00, 0x00, 0x00 },  /* Ľ to L */
  { 0x013E,  0x6C, 0x00, 0x00, 0x00 },  /* ľ to l */
  { 0x013F,  0x4C, 0x2E, 0x00, 0x00 },  /* Ŀ to L. */
  { 0x0140,  0x6C, 0x2E, 0x00, 0x00 },  /* ŀ to l. */
  { 0x0141,  0x4C, 0x00, 0x00, 0x00 },  /* Ł to L */
  { 0x0142,  0x6C, 0x00, 0x00, 0x00 },  /* ł to l */
  { 0x0143,  0x4E, 0x00, 0x00, 0x00 },  /* Ń to N */
  { 0x0144,  0x6E, 0x00, 0x00, 0x00 },  /* ń to n */
  { 0x0145,  0x4E, 0x00, 0x00, 0x00 },  /* Ņ to N */
  { 0x0146,  0x6E, 0x00, 0x00, 0x00 },  /* ņ to n */
  { 0x0147,  0x4E, 0x00, 0x00, 0x00 },  /* Ň to N */
  { 0x0148,  0x6E, 0x00, 0x00, 0x00 },  /* ň to n */
  { 0x0149,  0x27, 0x6E, 0x00, 0x00 },  /* ŉ to 'n */
  { 0x014A,  0x4E, 0x47, 0x00, 0x00 },  /* Ŋ to NG */
  { 0x014B,  0x6E, 0x67, 0x00, 0x00 },  /* ŋ to ng */
  { 0x014C,  0x4F, 0x00, 0x00, 0x00 },  /* Ō to O */
  { 0x014D,  0x6F, 0x00, 0x00, 0x00 },  /* ō to o */
  { 0x014E,  0x4F, 0x00, 0x00, 0x00 },  /* Ŏ to O */
  { 0x014F,  0x6F, 0x00, 0x00, 0x00 },  /* ŏ to o */
  { 0x0150,  0x4F, 0x00, 0x00, 0x00 },  /* Ő to O */
  { 0x0151,  0x6F, 0x00, 0x00, 0x00 },  /* ő to o */
  { 0x0152,  0x4F, 0x45, 0x00, 0x00 },  /* Œ to OE */
  { 0x0153,  0x6F, 0x65, 0x00, 0x00 },  /* œ to oe */
  { 0x0154,  0x52, 0x00, 0x00, 0x00 },  /* Ŕ to R */
  { 0x0155,  0x72, 0x00, 0x00, 0x00 },  /* ŕ to r */
  { 0x0156,  0x52, 0x00, 0x00, 0x00 },  /* Ŗ to R */
  { 0x0157,  0x72, 0x00, 0x00, 0x00 },  /* ŗ to r */
  { 0x0158,  0x52, 0x00, 0x00, 0x00 },  /* Ř to R */
  { 0x0159,  0x72, 0x00, 0x00, 0x00 },  /* ř to r */
  { 0x015A,  0x53, 0x00, 0x00, 0x00 },  /* Ś to S */
  { 0x015B,  0x73, 0x00, 0x00, 0x00 },  /* ś to s */
  { 0x015C,  0x53, 0x68, 0x00, 0x00 },  /* Ŝ to Sh */
  { 0x015D,  0x73, 0x68, 0x00, 0x00 },  /* ŝ to sh */
  { 0x015E,  0x53, 0x00, 0x00, 0x00 },  /* Ş to S */
  { 0x015F,  0x73, 0x00, 0x00, 0x00 },  /* ş to s */
  { 0x0160,  0x53, 0x00, 0x00, 0x00 },  /* Š to S */
  { 0x0161,  0x73, 0x00, 0x00, 0x00 },  /* š to s */
  { 0x0162,  0x54, 0x00, 0x00, 0x00 },  /* Ţ to T */
  { 0x0163,  0x74, 0x00, 0x00, 0x00 },  /* ţ to t */
  { 0x0164,  0x54, 0x00, 0x00, 0x00 },  /* Ť to T */
  { 0x0165,  0x74, 0x00, 0x00, 0x00 },  /* ť to t */
  { 0x0166,  0x54, 0x00, 0x00, 0x00 },  /* Ŧ to T */
  { 0x0167,  0x74, 0x00, 0x00, 0x00 },  /* ŧ to t */
  { 0x0168,  0x55, 0x00, 0x00, 0x00 },  /* Ũ to U */
  { 0x0169,  0x75, 0x00, 0x00, 0x00 },  /* ũ to u */
  { 0x016A,  0x55, 0x00, 0x00, 0x00 },  /* Ū to U */
  { 0x016B,  0x75, 0x00, 0x00, 0x00 },  /* ū to u */
  { 0x016C,  0x55, 0x00, 0x00, 0x00 },  /* Ŭ to U */
  { 0x016D,  0x75, 0x00, 0x00, 0x00 },  /* ŭ to u */
  { 0x016E,  0x55, 0x00, 0x00, 0x00 },  /* Ů to U */
  { 0x016F,  0x75, 0x00, 0x00, 0x00 },  /* ů to u */
  { 0x0170,  0x55, 0x00, 0x00, 0x00 },  /* Ű to U */
  { 0x0171,  0x75, 0x00, 0x00, 0x00 },  /* ű to u */
  { 0x0172,  0x55, 0x00, 0x00, 0x00 },  /* Ų to U */
  { 0x0173,  0x75, 0x00, 0x00, 0x00 },  /* ų to u */
  { 0x0174,  0x57, 0x00, 0x00, 0x00 },  /* Ŵ to W */
  { 0x0175,  0x77, 0x00, 0x00, 0x00 },  /* ŵ to w */
  { 0x0176,  0x59, 0x00, 0x00, 0x00 },  /* Ŷ to Y */
  { 0x0177,  0x79, 0x00, 0x00, 0x00 },  /* ŷ to y */
  { 0x0178,  0x59, 0x00, 0x00, 0x00 },  /* Ÿ to Y */
  { 0x0179,  0x5A, 0x00, 0x00, 0x00 },  /* Ź to Z */
  { 0x017A,  0x7A, 0x00, 0x00, 0x00 },  /* ź to z */
  { 0x017B,  0x5A, 0x00, 0x00, 0x00 },  /* Ż to Z */
  { 0x017C,  0x7A, 0x00, 0x00, 0x00 },  /* ż to z */
  { 0x017D,  0x5A, 0x00, 0x00, 0x00 },  /* Ž to Z */
  { 0x017E,  0x7A, 0x00, 0x00, 0x00 },  /* ž to z */
  { 0x017F,  0x73, 0x00, 0x00, 0x00 },  /* ſ to s */
  { 0x0192,  0x66, 0x00, 0x00, 0x00 },  /* ƒ to f */
  { 0x0218,  0x53, 0x00, 0x00, 0x00 },  /* Ș to S */
  { 0x0219,  0x73, 0x00, 0x00, 0x00 },  /* ș to s */
  { 0x021A,  0x54, 0x00, 0x00, 0x00 },  /* Ț to T */
  { 0x021B,  0x74, 0x00, 0x00, 0x00 },  /* ț to t */
  { 0x0386,  0x41, 0x00, 0x00, 0x00 },  /* Ά to A */
  { 0x0388,  0x45, 0x00, 0x00, 0x00 },  /* Έ to E */
  { 0x0389,  0x49, 0x00, 0x00, 0x00 },  /* Ή to I */
  { 0x038A,  0x49, 0x00, 0x00, 0x00 },  /* Ί to I */
  { 0x038C,  0x4f, 0x00, 0x00, 0x00 },  /* Ό to O */
  { 0x038E,  0x59, 0x00, 0x00, 0x00 },  /* Ύ to Y */
  { 0x038F,  0x4f, 0x00, 0x00, 0x00 },  /* Ώ to O */
  { 0x0390,  0x69, 0x00, 0x00, 0x00 },  /* ΐ to i */
  { 0x0391,  0x41, 0x00, 0x00, 0x00 },  /* Α to A */
  { 0x0392,  0x42, 0x00, 0x00, 0x00 },  /* Β to B */
  { 0x0393,  0x47, 0x00, 0x00, 0x00 },  /* Γ to G */
  { 0x0394,  0x44, 0x00, 0x00, 0x00 },  /* Δ to D */
  { 0x0395,  0x45, 0x00, 0x00, 0x00 },  /* Ε to E */
  { 0x0396,  0x5a, 0x00, 0x00, 0x00 },  /* Ζ to Z */
  { 0x0397,  0x49, 0x00, 0x00, 0x00 },  /* Η to I */
  { 0x0398,  0x54, 0x68, 0x00, 0x00 },  /* Θ to Th */
  { 0x0399,  0x49, 0x00, 0x00, 0x00 },  /* Ι to I */
  { 0x039A,  0x4b, 0x00, 0x00, 0x00 },  /* Κ to K */
  { 0x039B,  0x4c, 0x00, 0x00, 0x00 },  /* Λ to L */
  { 0x039C,  0x4d, 0x00, 0x00, 0x00 },  /* Μ to M */
  { 0x039D,  0x4e, 0x00, 0x00, 0x00 },  /* Ν to N */
  { 0x039E,  0x58, 0x00, 0x00, 0x00 },  /* Ξ to X */
  { 0x039F,  0x4f, 0x00, 0x00, 0x00 },  /* Ο to O */
  { 0x03A0,  0x50, 0x00, 0x00, 0x00 },  /* Π to P */
  { 0x03A1,  0x52, 0x00, 0x00, 0x00 },  /* Ρ to R */
  { 0x03A3,  0x53, 0x00, 0x00, 0x00 },  /* Σ to S */
  { 0x03A4,  0x54, 0x00, 0x00, 0x00 },  /* Τ to T */
  { 0x03A5,  0x59, 0x00, 0x00, 0x00 },  /* Υ to Y */
  { 0x03A6,  0x46, 0x00, 0x00, 0x00 },  /* Φ to F */
  { 0x03A7,  0x43, 0x68, 0x00, 0x00 },  /* Χ to Ch */
  { 0x03A8,  0x50, 0x73, 0x00, 0x00 },  /* Ψ to Ps */
  { 0x03A9,  0x4f, 0x00, 0x00, 0x00 },  /* Ω to O */
  { 0x03AA,  0x49, 0x00, 0x00, 0x00 },  /* Ϊ to I */
  { 0x03AB,  0x59, 0x00, 0x00, 0x00 },  /* Ϋ to Y */
  { 0x03AC,  0x61, 0x00, 0x00, 0x00 },  /* ά to a */
  { 0x03AD,  0x65, 0x00, 0x00, 0x00 },  /* έ to e */
  { 0x03AE,  0x69, 0x00, 0x00, 0x00 },  /* ή to i */
  { 0x03AF,  0x69, 0x00, 0x00, 0x00 },  /* ί to i */
  { 0x03B1,  0x61, 0x00, 0x00, 0x00 },  /* α to a */
  { 0x03B2,  0x62, 0x00, 0x00, 0x00 },  /* β to b */
  { 0x03B3,  0x67, 0x00, 0x00, 0x00 },  /* γ to g */
  { 0x03B4,  0x64, 0x00, 0x00, 0x00 },  /* δ to d */
  { 0x03B5,  0x65, 0x00, 0x00, 0x00 },  /* ε to e */
  { 0x03B6,  0x7a, 0x00, 0x00, 0x00 },  /* ζ to z */
  { 0x03B7,  0x69, 0x00, 0x00, 0x00 },  /* η to i */
  { 0x03B8,  0x74, 0x68, 0x00, 0x00 },  /* θ to th */
  { 0x03B9,  0x69, 0x00, 0x00, 0x00 },  /* ι to i */
  { 0x03BA,  0x6b, 0x00, 0x00, 0x00 },  /* κ to k */
  { 0x03BB,  0x6c, 0x00, 0x00, 0x00 },  /* λ to l */
  { 0x03BC,  0x6d, 0x00, 0x00, 0x00 },  /* μ to m */
  { 0x03BD,  0x6e, 0x00, 0x00, 0x00 },  /* ν to n */
  { 0x03BE,  0x78, 0x00, 0x00, 0x00 },  /* ξ to x */
  { 0x03BF,  0x6f, 0x00, 0x00, 0x00 },  /* ο to o */
  { 0x03C0,  0x70, 0x00, 0x00, 0x00 },  /* π to p */
  { 0x03C1,  0x72, 0x00, 0x00, 0x00 },  /* ρ to r */
  { 0x03C3,  0x73, 0x00, 0x00, 0x00 },  /* σ to s */
  { 0x03C4,  0x74, 0x00, 0x00, 0x00 },  /* τ to t */
  { 0x03C5,  0x79, 0x00, 0x00, 0x00 },  /* υ to y */
  { 0x03C6,  0x66, 0x00, 0x00, 0x00 },  /* φ to f */
  { 0x03C7,  0x63, 0x68, 0x00, 0x00 },  /* χ to ch */
  { 0x03C8,  0x70, 0x73, 0x00, 0x00 },  /* ψ to ps */
  { 0x03C9,  0x6f, 0x00, 0x00, 0x00 },  /* ω to o */
  { 0x03CA,  0x69, 0x00, 0x00, 0x00 },  /* ϊ to i */
  { 0x03CB,  0x79, 0x00, 0x00, 0x00 },  /* ϋ to y */
  { 0x03CC,  0x6f, 0x00, 0x00, 0x00 },  /* ό to o */
  { 0x03CD,  0x79, 0x00, 0x00, 0x00 },  /* ύ to y */
  { 0x03CE,  0x69, 0x00, 0x00, 0x00 },  /* ώ to i */
  { 0x0400,  0x45, 0x00, 0x00, 0x00 },  /* Ѐ to E */
  { 0x0401,  0x45, 0x00, 0x00, 0x00 },  /* Ё to E */
  { 0x0402,  0x44, 0x00, 0x00, 0x00 },  /* Ђ to D */
  { 0x0403,  0x47, 0x00, 0x00, 0x00 },  /* Ѓ to G */
  { 0x0404,  0x45, 0x00, 0x00, 0x00 },  /* Є to E */
  { 0x0405,  0x5a, 0x00, 0x00, 0x00 },  /* Ѕ to Z */
  { 0x0406,  0x49, 0x00, 0x00, 0x00 },  /* І to I */
  { 0x0407,  0x49, 0x00, 0x00, 0x00 },  /* Ї to I */
  { 0x0408,  0x4a, 0x00, 0x00, 0x00 },  /* Ј to J */
  { 0x0409,  0x49, 0x00, 0x00, 0x00 },  /* Љ to I */
  { 0x040A,  0x4e, 0x00, 0x00, 0x00 },  /* Њ to N */
  { 0x040B,  0x44, 0x00, 0x00, 0x00 },  /* Ћ to D */
  { 0x040C,  0x4b, 0x00, 0x00, 0x00 },  /* Ќ to K */
  { 0x040D,  0x49, 0x00, 0x00, 0x00 },  /* Ѝ to I */
  { 0x040E,  0x55, 0x00, 0x00, 0x00 },  /* Ў to U */
  { 0x040F,  0x44, 0x00, 0x00, 0x00 },  /* Џ to D */
  { 0x0410,  0x41, 0x00, 0x00, 0x00 },  /* А to A */
  { 0x0411,  0x42, 0x00, 0x00, 0x00 },  /* Б to B */
  { 0x0412,  0x56, 0x00, 0x00, 0x00 },  /* В to V */
  { 0x0413,  0x47, 0x00, 0x00, 0x00 },  /* Г to G */
  { 0x0414,  0x44, 0x00, 0x00, 0x00 },  /* Д to D */
  { 0x0415,  0x45, 0x00, 0x00, 0x00 },  /* Е to E */
  { 0x0416,  0x5a, 0x68, 0x00, 0x00 },  /* Ж to Zh */
  { 0x0417,  0x5a, 0x00, 0x00, 0x00 },  /* З to Z */
  { 0x0418,  0x49, 0x00, 0x00, 0x00 },  /* И to I */
  { 0x0419,  0x49, 0x00, 0x00, 0x00 },  /* Й to I */
  { 0x041A,  0x4b, 0x00, 0x00, 0x00 },  /* К to K */
  { 0x041B,  0x4c, 0x00, 0x00, 0x00 },  /* Л to L */
  { 0x041C,  0x4d, 0x00, 0x00, 0x00 },  /* М to M */
  { 0x041D,  0x4e, 0x00, 0x00, 0x00 },  /* Н to N */
  { 0x041E,  0x4f, 0x00, 0x00, 0x00 },  /* О to O */
  { 0x041F,  0x50, 0x00, 0x00, 0x00 },  /* П to P */
  { 0x0420,  0x52, 0x00, 0x00, 0x00 },  /* Р to R */
  { 0x0421,  0x53, 0x00, 0x00, 0x00 },  /* С to S */
  { 0x0422,  0x54, 0x00, 0x00, 0x00 },  /* Т to T */
  { 0x0423,  0x55, 0x00, 0x00, 0x00 },  /* У to U */
  { 0x0424,  0x46, 0x00, 0x00, 0x00 },  /* Ф to F */
  { 0x0425,  0x4b, 0x68, 0x00, 0x00 },  /* Х to Kh */
  { 0x0426,  0x54, 0x63, 0x00, 0x00 },  /* Ц to Tc */
  { 0x0427,  0x43, 0x68, 0x00, 0x00 },  /* Ч to Ch */
  { 0x0428,  0x53, 0x68, 0x00, 0x00 },  /* Ш to Sh */
  { 0x0429,  0x53, 0x68, 0x63, 0x68 },  /* Щ to Shch */
  { 0x042A,  0x61, 0x00, 0x00, 0x00 },  /*  to A */
  { 0x042B,  0x59, 0x00, 0x00, 0x00 },  /* Ы to Y */
  { 0x042C,  0x59, 0x00, 0x00, 0x00 },  /*  to Y */
  { 0x042D,  0x45, 0x00, 0x00, 0x00 },  /* Э to E */
  { 0x042E,  0x49, 0x75, 0x00, 0x00 },  /* Ю to Iu */
  { 0x042F,  0x49, 0x61, 0x00, 0x00 },  /* Я to Ia */
  { 0x0430,  0x61, 0x00, 0x00, 0x00 },  /* а to a */
  { 0x0431,  0x62, 0x00, 0x00, 0x00 },  /* б to b */
  { 0x0432,  0x76, 0x00, 0x00, 0x00 },  /* в to v */
  { 0x0433,  0x67, 0x00, 0x00, 0x00 },  /* г to g */
  { 0x0434,  0x64, 0x00, 0x00, 0x00 },  /* д to d */
  { 0x0435,  0x65, 0x00, 0x00, 0x00 },  /* е to e */
  { 0x0436,  0x7a, 0x68, 0x00, 0x00 },  /* ж to zh */
  { 0x0437,  0x7a, 0x00, 0x00, 0x00 },  /* з to z */
  { 0x0438,  0x69, 0x00, 0x00, 0x00 },  /* и to i */
  { 0x0439,  0x69, 0x00, 0x00, 0x00 },  /* й to i */
  { 0x043A,  0x6b, 0x00, 0x00, 0x00 },  /* к to k */
  { 0x043B,  0x6c, 0x00, 0x00, 0x00 },  /* л to l */
  { 0x043C,  0x6d, 0x00, 0x00, 0x00 },  /* м to m */
  { 0x043D,  0x6e, 0x00, 0x00, 0x00 },  /* н to n */
  { 0x043E,  0x6f, 0x00, 0x00, 0x00 },  /* о to o */
  { 0x043F,  0x70, 0x00, 0x00, 0x00 },  /* п to p */
  { 0x0440,  0x72, 0x00, 0x00, 0x00 },  /* р to r */
  { 0x0441,  0x73, 0x00, 0x00, 0x00 },  /* с to s */
  { 0x0442,  0x74, 0x00, 0x00, 0x00 },  /* т to t */
  { 0x0443,  0x75, 0x00, 0x00, 0x00 },  /* у to u */
  { 0x0444,  0x66, 0x00, 0x00, 0x00 },  /* ф to f */
  { 0x0445,  0x6b, 0x68, 0x00, 0x00 },  /* х to kh */
  { 0x0446,  0x74, 0x63, 0x00, 0x00 },  /* ц to tc */
  { 0x0447,  0x63, 0x68, 0x00, 0x00 },  /* ч to ch */
  { 0x0448,  0x73, 0x68, 0x00, 0x00 },  /* ш to sh */
  { 0x0449,  0x73, 0x68, 0x63, 0x68 },  /* щ to shch */
  { 0x044A,  0x61, 0x00, 0x00, 0x00 },  /*  to a */
  { 0x044B,  0x79, 0x00, 0x00, 0x00 },  /* ы to y */
  { 0x044C,  0x79, 0x00, 0x00, 0x00 },  /*  to y */
  { 0x044D,  0x65, 0x00, 0x00, 0x00 },  /* э to e */
  { 0x044E,  0x69, 0x75, 0x00, 0x00 },  /* ю to iu */
  { 0x044F,  0x69, 0x61, 0x00, 0x00 },  /* я to ia */
  { 0x0450,  0x65, 0x00, 0x00, 0x00 },  /* ѐ to e */
  { 0x0451,  0x65, 0x00, 0x00, 0x00 },  /* ё to e */
  { 0x0452,  0x64, 0x00, 0x00, 0x00 },  /* ђ to d */
  { 0x0453,  0x67, 0x00, 0x00, 0x00 },  /* ѓ to g */
  { 0x0454,  0x65, 0x00, 0x00, 0x00 },  /* є to e */
  { 0x0455,  0x7a, 0x00, 0x00, 0x00 },  /* ѕ to z */
  { 0x0456,  0x69, 0x00, 0x00, 0x00 },  /* і to i */
  { 0x0457,  0x69, 0x00, 0x00, 0x00 },  /* ї to i */
  { 0x0458,  0x6a, 0x00, 0x00, 0x00 },  /* ј to j */
  { 0x0459,  0x69, 0x00, 0x00, 0x00 },  /* љ to i */
  { 0x045A,  0x6e, 0x00, 0x00, 0x00 },  /* њ to n */
  { 0x045B,  0x64, 0x00, 0x00, 0x00 },  /* ћ to d */
  { 0x045C,  0x6b, 0x00, 0x00, 0x00 },  /* ќ to k */
  { 0x045D,  0x69, 0x00, 0x00, 0x00 },  /* ѝ to i */
  { 0x045E,  0x75, 0x00, 0x00, 0x00 },  /* ў to u */
  { 0x045F,  0x64, 0x00, 0x00, 0x00 },  /* џ to d */
  { 0x1E02,  0x42, 0x00, 0x00, 0x00 },  /* Ḃ to B */
  { 0x1E03,  0x62, 0x00, 0x00, 0x00 },  /* ḃ to b */
  { 0x1E0A,  0x44, 0x00, 0x00, 0x00 },  /* Ḋ to D */
  { 0x1E0B,  0x64, 0x00, 0x00, 0x00 },  /* ḋ to d */
  { 0x1E1E,  0x46, 0x00, 0x00, 0x00 },  /* Ḟ to F */
  { 0x1E1F,  0x66, 0x00, 0x00, 0x00 },  /* ḟ to f */
  { 0x1E40,  0x4D, 0x00, 0x00, 0x00 },  /* Ṁ to M */
  { 0x1E41,  0x6D, 0x00, 0x00, 0x00 },  /* ṁ to m */
  { 0x1E56,  0x50, 0x00, 0x00, 0x00 },  /* Ṗ to P */
  { 0x1E57,  0x70, 0x00, 0x00, 0x00 },  /* ṗ to p */
  { 0x1E60,  0x53, 0x00, 0x00, 0x00 },  /* Ṡ to S */
  { 0x1E61,  0x73, 0x00, 0x00, 0x00 },  /* ṡ to s */
  { 0x1E6A,  0x54, 0x00, 0x00, 0x00 },  /* Ṫ to T */
  { 0x1E6B,  0x74, 0x00, 0x00, 0x00 },  /* ṫ to t */
  { 0x1E80,  0x57, 0x00, 0x00, 0x00 },  /* Ẁ to W */
  { 0x1E81,  0x77, 0x00, 0x00, 0x00 },  /* ẁ to w */
  { 0x1E82,  0x57, 0x00, 0x00, 0x00 },  /* Ẃ to W */
  { 0x1E83,  0x77, 0x00, 0x00, 0x00 },  /* ẃ to w */
  { 0x1E84,  0x57, 0x00, 0x00, 0x00 },  /* Ẅ to W */
  { 0x1E85,  0x77, 0x00, 0x00, 0x00 },  /* ẅ to w */
  { 0x1EF2,  0x59, 0x00, 0x00, 0x00 },  /* Ỳ to Y */
  { 0x1EF3,  0x79, 0x00, 0x00, 0x00 },  /* ỳ to y */
  { 0xFB00,  0x66, 0x66, 0x00, 0x00 },  /* ﬀ to ff */
  { 0xFB01,  0x66, 0x69, 0x00, 0x00 },  /* ﬁ to fi */
  { 0xFB02,  0x66, 0x6C, 0x00, 0x00 },  /* ﬂ to fl */
  { 0xFB05,  0x73, 0x74, 0x00, 0x00 },  /* ﬅ to st */
  { 0xFB06,  0x73, 0x74, 0x00, 0x00 },  /* ﬆ to st */
};

static const transliteration_t *__spellfix_find_translit(int c, int *px_top){
  *px_top = (sizeof(translit)/sizeof(translit[0])) - 1;
  return translit;
}

/*
** Convert the input string from UTF-8 into pure ASCII by converting
** all non-ASCII characters to some combination of characters in the
** ASCII subset.
**
** The returned string might contain more characters than the input.
**
** Space to hold the returned string comes from dbsql_malloc() and
** should be freed by the caller.
*/
static unsigned char *transliterate(const unsigned char *in, int n_in){
#ifdef DBSQL_SPELLFIX_5BYTE_MAPPINGS
  unsigned char *out = dbsql_malloc64( (int64_t)n_in*5 + 1 );
#else
  unsigned char *out = dbsql_malloc64( (int64_t)n_in*4 + 1 );
#endif
  int c, sz, n_out;
  if( out==0 ) return 0;
  n_out = 0;
  while( n_in>0 ){
    c = utf8Read(in, n_in, &sz);
    in += sz;
    n_in -= sz;
    if( c<=127 ){
      out[n_out++] = (unsigned char)c;
    }else{
      int top, btm, x;
      const transliteration_t *tbl = __spellfix_find_translit(c, &top);
      btm = 0;
      while( top>=btm ){
        x = (top + btm)/2;
        if( tbl[x].c_from==c ){
          out[n_out++] = tbl[x].c_to0;
          if( tbl[x].c_to1 ){
            out[n_out++] = tbl[x].c_to1;
            if( tbl[x].c_to2 ){
              out[n_out++] = tbl[x].c_to2;
              if( tbl[x].c_to3 ){
                out[n_out++] = tbl[x].c_to3;
#ifdef DBSQL_SPELLFIX_5BYTE_MAPPINGS
                if( tbl[x].c_to4 ){
                  out[n_out++] = tbl[x].c_to4;
                }
#endif /* DBSQL_SPELLFIX_5BYTE_MAPPINGS */
              }
            }
          }
          c = 0;
          break;
        }else if( tbl[x].c_from>c ){
          top = x-1;
        }else{
          btm = x+1;
        }
      }
      if( c ) out[n_out++] = '?';
    }
  }
  out[n_out] = 0;
  return out;
}

/*
** Return the number of characters in the shortest prefix of the input
** string that transliterates to an ASCII string n_trans bytes or longer.
** Or, if the transliteration of the input string is less than n_trans
** bytes in size, return the number of characters in the input string.
*/
static int translen_to_charlen(const char *in, int n_in, int n_trans){
  int i, c, sz, n_out;
  int n_char;

  i = n_out = 0;
  for(n_char=0; i<n_in && n_out<n_trans; n_char++){
    c = utf8Read((const unsigned char *)&in[i], n_in-i, &sz);
    i += sz;

    n_out++;
    if( c>=128 ){
      int top, btm, x;
      const transliteration_t *tbl = __spellfix_find_translit(c, &top);
      btm = 0;
      while( top>=btm ){
        x = (top + btm)/2;
        if( tbl[x].c_from==c ){
          if( tbl[x].c_to1 ){
            n_out++;
            if( tbl[x].c_to2 ){
              n_out++;
              if( tbl[x].c_to3 ){
                n_out++;
              }
            }
          }
          break;
        }else if( tbl[x].c_from>c ){
          top = x-1;
        }else{
          btm = x+1;
        }
      }
    }
  }

  return n_char;
}


/*
**    spellfix1_translit(X)
**
** Convert a string that contains non-ASCII Roman characters into 
** pure ASCII.
*/
static void __transliterate_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *in = dbsql_value_text(argv[0]);
  int n_in = dbsql_value_bytes(argv[0]);
  unsigned char *out = transliterate(in, n_in);
  if( out==0 ){
    dbsql_result_error_nomem(context);
  }else{
    dbsql_result_text(context, (char*)out, -1, dbsql_free);
  }
}

/*
**    spellfix1_scriptcode(X)
**
** Try to determine the dominant script used by the word X and return
** its ISO 15924 numeric code.
**
** The current implementation only understands the following scripts:
**
**    215  (Latin)
**    220  (Cyrillic)
**    200  (Greek)
**
** This routine will return 998 if the input X contains characters from
** two or more of the above scripts or 999 if X contains no characters
** from any of the above scripts.
*/
static void __script_code_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *in = dbsql_value_text(argv[0]);
  int n_in = dbsql_value_bytes(argv[0]);
  int c, sz;
  int script_mask = 0;
  int res;
  int seen_digit = 0;
# define SCRIPT_LATIN       0x0001
# define SCRIPT_CYRILLIC    0x0002
# define SCRIPT_GREEK       0x0004
# define SCRIPT_HEBREW      0x0008
# define SCRIPT_ARABIC      0x0010

  while( n_in>0 ){
    c = utf8Read(in, n_in, &sz);
    in += sz;
    n_in -= sz;
    if( c<0x02af ){
      if( c>=0x80 || __mid_class[c&0x7f]<CCLASS_DIGIT ){
        script_mask |= SCRIPT_LATIN;
      }else if( c>='0' && c<='9' ){
        seen_digit = 1;
      }
    }else if( c>=0x0400 && c<=0x04ff ){
      script_mask |= SCRIPT_CYRILLIC;
    }else if( c>=0x0386 && c<=0x03ce ){
      script_mask |= SCRIPT_GREEK;
    }else if( c>=0x0590 && c<=0x05ff ){
      script_mask |= SCRIPT_HEBREW;
    }else if( c>=0x0600 && c<=0x06ff ){
      script_mask |= SCRIPT_ARABIC;
    }
  }
  if( script_mask==0 && seen_digit ) script_mask = SCRIPT_LATIN;
  switch( script_mask ){
    case 0:                res = 999; break;
    case SCRIPT_LATIN:     res = 215; break;
    case SCRIPT_CYRILLIC:  res = 220; break;
    case SCRIPT_GREEK:     res = 200; break;
    case SCRIPT_HEBREW:    res = 125; break;
    case SCRIPT_ARABIC:    res = 160; break;
    default:               res = 998; break;
  }
  dbsql_result_int(context, res);
}

/* End transliterate
******************************************************************************
******************************************************************************
** Begin spellfix1 virtual table.
*/

/* Maximum length of a phonehash used for querying the shadow table */
#define SPELLFIX_MX_HASH  32

/* Maximum number of hash strings to examine per query */
#define SPELLFIX_MX_RUN   1

typedef struct spellfix1_vtab spellfix1_vtab;
typedef struct spellfix1_cursor spellfix1_cursor;

/* Fuzzy-search virtual table object */
struct spellfix1_vtab {
  dbsql_vtab_t base;         /* Base class - must be first */
  DBSQL *db;               /* Database connection */
  char *db_name;             /* Name of database holding this table */
  char *table_name;          /* Name of the virtual table */
  char *cost_table;          /* table_t holding edit-distance cost numbers */
  edit_dist3_config_t *config3; /* Parsed edit distance costs */
};

/* Fuzzy-search cursor object */
struct spellfix1_cursor {
  dbsql_vtab_cursor_t base;    /* Base class - must be first */
  spellfix1_vtab *v_tab;       /* The table to which this cursor belongs */
  char *pattern;              /* rhs of MATCH clause */
  int idx_num;                  /* idx_num value passed to filter() */
  int n_row;                    /* Number of rows of content */
  int n_alloc;                  /* Number of allocated rows */
  int i_row;                    /* Current row of content */
  int i_lang;                   /* Value of the langid= constraint */
  int i_top;                    /* Value of the top= constraint */
  int i_scope;                  /* Value of the scope= constraint */
  int n_search;                 /* Number of vocabulary items checked */
  dbsql_stmt_t *full_scan;     /* Shadow query for a full table scan */
  struct spellfix1_row {       /* For each row of content */
    int64_t i_rowid;         /* Rowid for this row */
    char *word;                  /* Text for this row */
    int i_rank;                    /* Rank for this row */
    int i_distance;                /* Distance from pattern for this row */
    int i_score;                   /* Score for sorting */
    int i_matchlen;                /* Value of matchlen column (or -1) */
    char hash[SPELLFIX_MX_HASH]; /* the phonehash used for this match */
  } *a; 
};

/*
** Construct one or more SQL statements from the format string given
** and then evaluate those statements. The success code is written
** into *rc.
**
** If *rc is initially non-zero then this routine is a no-op.
*/
static void __spellfix1_db_exec(
  int *rc,              /* Success code */
  DBSQL *db,           /* Database in which to run SQL */
  const char *format,   /* Format string for SQL */
  ...                    /* Arguments to the format string */
){
  va_list ap;
  char *sql;
  if( *rc ) return;
  va_start(ap, format);
  sql = dbsql_vmprintf(format, ap);
  va_end(ap);
  if( sql==0 ){
    *rc = DBSQL_NOMEM;
  }else{
    *rc = dbsql_exec(db, sql, 0, 0, 0);
    dbsql_free(sql);
  }
}

/*
** disconnect/destroy method for the fuzzy-search module.
*/
static int spellfix1Uninit(int is_destroy, dbsql_vtab_t *v_tab){
  spellfix1_vtab *p = (spellfix1_vtab*)v_tab;
  int rc = DBSQL_OK;
  if( is_destroy ){
    DBSQL *db = p->db;
    __spellfix1_db_exec(&rc, db, "DROP TABLE IF EXISTS \"%w\".\"%w_vocab\"",
                  p->db_name, p->table_name);
  }
  if( rc==DBSQL_OK ){
    dbsql_free(p->table_name);
    __edit_dist3_config_delete(p->config3);
    dbsql_free(p->cost_table);
    dbsql_free(p);
  }
  return rc;
}
static int spellfix1Disconnect(dbsql_vtab_t *v_tab){
  return spellfix1Uninit(0, v_tab);
}
static int spellfix1Destroy(dbsql_vtab_t *v_tab){
  return spellfix1Uninit(1, v_tab);
}

/*
** Make a copy of a string.  Remove leading and trailing whitespace
** and dequote it.
*/
static char *spellfix1Dequote(const char *in){
  char *out;
  int i, j;
  char c;
  while( isspace((unsigned char)in[0]) ) in++;
  out = dbsql_mprintf("%s", in);
  if( out==0 ) return 0;
  i = (int)strlen(out);
  /* The parser should never leave spaces at the end, but we'll
  ** remove them just in case. */
  while( i>0 && isspace(out[i-1]) ){ i--; }
  out[i] = 0;
  c = out[0];
  if( c=='\'' || c=='"' ){
    for(i=1, j=0; out[i]; i++){
      out[j++] = out[i];
      if( out[i]==c ){
        if( out[i+1]==c ){
          i++;
        }else{
          out[j-1] = 0;
          break;
        }
      }
    }
  }
  return out;
}


/*
** connect/create method for the spellfix1 module. Arguments are:
**
**   argv[0]   -> module name  ("spellfix1")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[3].. -> optional arguments (i.e. "edit_cost_table" parameter)
*/
static int spellfix1Init(
  int is_create,
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **v_tab,
  char **err_p
){
  spellfix1_vtab *p_new = 0;
  /* const char *module = argv[0]; // not used */
  const char *db_name = argv[1];
  const char *table_name = argv[2];
  int n_db_name;
  int rc = DBSQL_OK;
  int i;

  n_db_name = (int)strlen(db_name);
  p_new = dbsql_malloc64( sizeof(*p_new) + (int64_t)n_db_name + 1);
  if( p_new==0 ){
    rc = DBSQL_NOMEM;
  }else{
    memset(p_new, 0, sizeof(*p_new));
    p_new->db_name = (char*)&p_new[1];
    memcpy(p_new->db_name, db_name, n_db_name+1);
    p_new->table_name = dbsql_mprintf("%s", table_name);
    p_new->db = db;
    if( p_new->table_name==0 ){
      rc = DBSQL_NOMEM;
    }else{
      dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
      rc = dbsql_declare_vtab(db, 
           "CREATE TABLE x(word,rank,distance,langid, "
           "score, matchlen, phonehash HIDDEN, "
           "top HIDDEN, scope HIDDEN, srchcnt HIDDEN, "
           "soundslike HIDDEN, command HIDDEN)"
      );
#define SPELLFIX_COL_WORD            0
#define SPELLFIX_COL_RANK            1
#define SPELLFIX_COL_DISTANCE        2
#define SPELLFIX_COL_LANGID          3
#define SPELLFIX_COL_SCORE           4
#define SPELLFIX_COL_MATCHLEN        5
#define SPELLFIX_COL_PHONEHASH       6
#define SPELLFIX_COL_TOP             7
#define SPELLFIX_COL_SCOPE           8
#define SPELLFIX_COL_SRCHCNT         9
#define SPELLFIX_COL_SOUNDSLIKE     10
#define SPELLFIX_COL_COMMAND        11
    }
    if( rc==DBSQL_OK && is_create ){
      __spellfix1_db_exec(&rc, db,
         "CREATE TABLE IF NOT EXISTS \"%w\".\"%w_vocab\"(\n"
         "  id INTEGER PRIMARY KEY,\n"
         "  rank INT,\n"
         "  langid INT,\n"
         "  word TEXT,\n"
         "  k1 TEXT,\n"
         "  k2 TEXT\n"
         ");\n",
         db_name, table_name
      );
      __spellfix1_db_exec(&rc, db,
         "CREATE INDEX IF NOT EXISTS \"%w\".\"%w_vocab_index_langid_k2\" "
            "ON \"%w_vocab\"(langid,k2);",
         db_name, table_name, table_name
      );
    }
    for(i=3; rc==DBSQL_OK && i<argc; i++){
      if( strncmp(argv[i],"edit_cost_table=",16)==0 && p_new->cost_table==0 ){
        p_new->cost_table = spellfix1Dequote(&argv[i][16]);
        if( p_new->cost_table==0 ) rc = DBSQL_NOMEM;
        continue;
      }
      *err_p = dbsql_mprintf("bad argument to spellfix1(): \"%s\"", argv[i]);
      rc = DBSQL_ERROR; 
    }
  }

  if( rc && p_new ){
    *v_tab = 0;
    spellfix1Uninit(0, &p_new->base);
  }else{
    *v_tab = (dbsql_vtab_t *)p_new;
  }
  return rc;
}

/*
** The connect and create methods
*/
static int spellfix1Connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **v_tab,
  char **err_p
){
  return spellfix1Init(0, db, aux, argc, argv, v_tab, err_p);
}
static int spellfix1Create(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **v_tab,
  char **err_p
){
  return spellfix1Init(1, db, aux, argc, argv, v_tab, err_p);
}

/*
** Clear all of the content from a cursor.
*/
static void __spellfix1_reset_cursor(spellfix1_cursor *cur){
  int i;
  for(i=0; i<cur->n_row; i++){
    dbsql_free(cur->a[i].word);
  }
  cur->n_row = 0;
  cur->i_row = 0;
  cur->n_search = 0;
  if( cur->full_scan ){
    dbsql_finalize(cur->full_scan);
    cur->full_scan = 0;
  }
}

/*
** Resize the cursor to hold up to N rows of content
*/
static void __spellfix1_resize_cursor(spellfix1_cursor *cur, int N){
  struct spellfix1_row *news;
  assert( N>=cur->n_row );
  news = dbsql_realloc64(cur->a, sizeof(cur->a[0])*N);
  if( news==0 && N>0 ){
    __spellfix1_reset_cursor(cur);
    dbsql_free(cur->a);
    cur->n_alloc = 0;
    cur->a = 0;
  }else{
    cur->n_alloc = N;
    cur->a = news;
  }
}


/*
** Close a fuzzy-search cursor.
*/
static int spellfix1Close(dbsql_vtab_cursor_t *cur){
  spellfix1_cursor *p_cur = (spellfix1_cursor *)cur;
  __spellfix1_reset_cursor(p_cur);
  __spellfix1_resize_cursor(p_cur, 0);
  dbsql_free(p_cur->pattern);
  dbsql_free(p_cur);
  return DBSQL_OK;
}

#define SPELLFIX_IDXNUM_MATCH  0x01         /* word MATCH $str */
#define SPELLFIX_IDXNUM_LANGID 0x02         /* langid == $langid */
#define SPELLFIX_IDXNUM_TOP    0x04         /* top = $top */
#define SPELLFIX_IDXNUM_SCOPE  0x08         /* scope = $scope */
#define SPELLFIX_IDXNUM_DISTLT 0x10         /* distance < $distance */
#define SPELLFIX_IDXNUM_DISTLE 0x20         /* distance <= $distance */
#define SPELLFIX_IDXNUM_ROWID  0x40         /* rowid = $rowid */
#define SPELLFIX_IDXNUM_DIST   (0x10|0x20)  /* DISTLT and DISTLE */

/*
**
** The plan number is a bitmask of the SPELLFIX_IDXNUM_* values defined
** above.
**
** filter.argv[*] values contains $str, $langid, $top, $scope and $rowid
** if specified and in that order.
*/
static int __spellfix1_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int i_plan = 0;
  int i_lang_term = -1;
  int i_top_term = -1;
  int i_scope_term = -1;
  int i_dist_term = -1;
  int i_rowid_term = -1;
  int i;
  const struct dbsql_index_constraint_t *constraint;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->usable==0 ) continue;

    /* Terms of the form:  word MATCH $str */
    if( (i_plan & SPELLFIX_IDXNUM_MATCH)==0 
     && constraint->i_column==SPELLFIX_COL_WORD
     && constraint->op==DBSQL_INDEX_CONSTRAINT_MATCH
    ){
      i_plan |= SPELLFIX_IDXNUM_MATCH;
      idx_info->constraint_usages[i].argv_index = 1;
      idx_info->constraint_usages[i].omit = 1;
    }

    /* Terms of the form:  langid = $langid  */
    if( (i_plan & SPELLFIX_IDXNUM_LANGID)==0
     && constraint->i_column==SPELLFIX_COL_LANGID
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= SPELLFIX_IDXNUM_LANGID;
      i_lang_term = i;
    }

    /* Terms of the form:  top = $top */
    if( (i_plan & SPELLFIX_IDXNUM_TOP)==0
     && constraint->i_column==SPELLFIX_COL_TOP
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= SPELLFIX_IDXNUM_TOP;
      i_top_term = i;
    }

    /* Terms of the form:  scope = $scope */
    if( (i_plan & SPELLFIX_IDXNUM_SCOPE)==0
     && constraint->i_column==SPELLFIX_COL_SCOPE
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= SPELLFIX_IDXNUM_SCOPE;
      i_scope_term = i;
    }

    /* Terms of the form:  distance < $dist or distance <= $dist */
    if( (i_plan & SPELLFIX_IDXNUM_DIST)==0
     && constraint->i_column==SPELLFIX_COL_DISTANCE
     && (constraint->op==DBSQL_INDEX_CONSTRAINT_LT
          || constraint->op==DBSQL_INDEX_CONSTRAINT_LE)
    ){
      if( constraint->op==DBSQL_INDEX_CONSTRAINT_LT ){
        i_plan |= SPELLFIX_IDXNUM_DISTLT;
      }else{
        i_plan |= SPELLFIX_IDXNUM_DISTLE;
      }
      i_dist_term = i;
    }

    /* Terms of the form:  distance < $dist or distance <= $dist */
    if( (i_plan & SPELLFIX_IDXNUM_ROWID)==0
     && constraint->i_column<0
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= SPELLFIX_IDXNUM_ROWID;
      i_rowid_term = i;
    }
  }
  if( i_plan&SPELLFIX_IDXNUM_MATCH ){
    int idx = 2;
    idx_info->idx_num = i_plan;
    if( idx_info->n_order_by==1
     && idx_info->order_bies[0].i_column==SPELLFIX_COL_SCORE
     && idx_info->order_bies[0].desc==0
    ){
      idx_info->order_by_consumed = 1;  /* Default order by i_score */
    }
    if( i_plan&SPELLFIX_IDXNUM_LANGID ){
      idx_info->constraint_usages[i_lang_term].argv_index = idx++;
      idx_info->constraint_usages[i_lang_term].omit = 1;
    }
    if( i_plan&SPELLFIX_IDXNUM_TOP ){
      idx_info->constraint_usages[i_top_term].argv_index = idx++;
      idx_info->constraint_usages[i_top_term].omit = 1;
    }
    if( i_plan&SPELLFIX_IDXNUM_SCOPE ){
      idx_info->constraint_usages[i_scope_term].argv_index = idx++;
      idx_info->constraint_usages[i_scope_term].omit = 1;
    }
    if( i_plan&SPELLFIX_IDXNUM_DIST ){
      idx_info->constraint_usages[i_dist_term].argv_index = idx++;
      idx_info->constraint_usages[i_dist_term].omit = 1;
    }
    idx_info->estimated_cost = 1e5;
  }else if( (i_plan & SPELLFIX_IDXNUM_ROWID) ){
    idx_info->idx_num = SPELLFIX_IDXNUM_ROWID;
    idx_info->constraint_usages[i_rowid_term].argv_index = 1;
    idx_info->constraint_usages[i_rowid_term].omit = 1;
    idx_info->estimated_cost = 5;
  }else{
    idx_info->idx_num = 0;
    idx_info->estimated_cost = 1e50;
  }
  return DBSQL_OK;
}

/*
** Open a new fuzzy-search cursor.
*/
static int spellfix1Open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  spellfix1_vtab *p = (spellfix1_vtab*)v_tab;
  spellfix1_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->v_tab = p;
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Adjust a distance measurement by the words rank in order to show
** preference to common words.
*/
static int spellfix1Score(int i_distance, int i_rank){
  int i_log2;
  for(i_log2=0; i_rank>0; i_log2++, i_rank>>=1){}
  return i_distance + 32 - i_log2;
}

/*
** Compare two spellfix1_row objects for sorting purposes in qsort() such
** that they sort in order of increasing distance.
*/
static int DBSQL_CDECL __spellfix1_row_compare(const void *A, const void *B){
  const struct spellfix1_row *a = (const struct spellfix1_row*)A;
  const struct spellfix1_row *b = (const struct spellfix1_row*)B;
  return a->i_score - b->i_score;
}

/*
** A structure used to pass information from __spellfix1_filter_for_match()
** into __spellfix1_run_query().
*/
typedef struct match_query_t {
  spellfix1_cursor *cur;          /* The cursor being queried */
  dbsql_stmt_t *stmt;             /* shadow table query statment */
  char hash[SPELLFIX_MX_HASH];    /* The current phonehash for pattern */
  const char *pattern;            /* Transliterated input string */
  int n_pattern;                    /* Length of pattern */
  edit_dist3_from_string_t *match_str3; /* Original unicode string */
  edit_dist3_config_t *config3;       /* Edit-distance cost coefficients */
  const edit_dist3_lang_t *lang;      /* The selected language coefficients */
  int i_lang;                       /* The language id */
  int i_scope;                      /* Default scope */
  int i_max_dist;                    /* Maximum allowed edit distance, or -1 */
  int rc;                          /* Error code */
  int n_run;                  /* Number of prior runs for the same pattern */
  char priors[SPELLFIX_MX_RUN][SPELLFIX_MX_HASH];  /* Prior hashes */
} match_query_t;

/*
** Run a query looking for the best matches against pattern using
** hash as the character class seed hash.
*/
static void __spellfix1_run_query(match_query_t *p, const char *query, int n_query){
  const char *k1;
  const char *word;
  int i_dist;
  int i_rank;
  int i_score;
  int i_worst = 0;
  int idx;
  int idx_worst = -1;
  int i;
  int i_scope = p->i_scope;
  spellfix1_cursor *cur = p->cur;
  dbsql_stmt_t *stmt = p->stmt;
  char hash1[SPELLFIX_MX_HASH];
  char hash2[SPELLFIX_MX_HASH];
  char *z_class;
  int n_class;
  int rc;

  if( cur->a==0 || p->rc ) return;   /* Prior memory allocation failure */
  z_class = (char*)__phonetic_hash((unsigned char*)query, n_query);
  if( z_class==0 ){
    p->rc = DBSQL_NOMEM;
    return;
  }
  n_class = (int)strlen(z_class);
  if( n_class>SPELLFIX_MX_HASH-2 ){
    n_class = SPELLFIX_MX_HASH-2;
    z_class[n_class] = 0;
  }
  if( n_class<=i_scope ){
    if( n_class>2 ){
      i_scope = n_class-1;
    }else{
      i_scope = n_class;
    }
  }
  memcpy(hash1, z_class, i_scope);
  dbsql_free(z_class);
  hash1[i_scope] = 0;
  memcpy(hash2, hash1, i_scope);
  hash2[i_scope] = 'Z';
  hash2[i_scope+1] = 0;
#if SPELLFIX_MX_RUN>1
  for(i=0; i<p->n_run; i++){
    if( strcmp(p->priors[i], hash1)==0 ) return;
  }
#endif
  assert( p->n_run<SPELLFIX_MX_RUN );
  memcpy(p->priors[p->n_run++], hash1, i_scope+1);
  if( dbsql_bind_text(stmt, 1, hash1, -1, DBSQL_STATIC)==DBSQL_NOMEM
   || dbsql_bind_text(stmt, 2, hash2, -1, DBSQL_STATIC)==DBSQL_NOMEM
  ){
    p->rc = DBSQL_NOMEM;
    return;
  }
#if SPELLFIX_MX_RUN>1
  for(i=0; i<cur->n_row; i++){
    if( cur->a[i].i_score>i_worst ){
      i_worst = cur->a[i].i_score;
      idx_worst = i;
    }
  }
#endif
  while( dbsql_step(stmt)==DBSQL_ROW ){
    int i_matchlen = -1;
    i_rank = dbsql_column_int(stmt, 2);
    if( p->match_str3 ){
      int n_word = dbsql_column_bytes(stmt, 1);
      word = (const char*)dbsql_column_text(stmt, 1);
      i_dist = __edit_dist3_core(p->match_str3, word, n_word, p->lang, &i_matchlen);
    }else{
      k1 = (const char*)dbsql_column_text(stmt, 3);
      if( k1==0 ) continue;
      i_dist = editdist1(p->pattern, k1, 0);
    }
    if( i_dist<0 ){
      p->rc = i_dist==(-4) ? DBSQL_TOOBIG : DBSQL_NOMEM;
      break;
    }
    cur->n_search++;
    
    /* If there is a "distance < $dist" or "distance <= $dist" constraint,
    ** check if this row meets it. If not, jump back up to the top of the
    ** loop to process the next row. Otherwise, if the row does match the
    ** distance constraint, check if the cur->a[] array is already full.
    ** If it is and no explicit "top = ?" constraint was present in the
    ** query, grow the array to ensure there is room for the new entry. */
    assert( (p->i_max_dist>=0)==((cur->idx_num & SPELLFIX_IDXNUM_DIST) ? 1 : 0) );
    if( p->i_max_dist>=0 ){
      if( i_dist>p->i_max_dist ) continue;
      if( cur->n_row>=cur->n_alloc && (cur->idx_num & SPELLFIX_IDXNUM_TOP)==0 ){
        __spellfix1_resize_cursor(cur, cur->n_alloc*2 + 10);
        if( cur->a==0 ) break;
      }
    }

    i_score = spellfix1Score(i_dist,i_rank);
    if( cur->n_row<cur->n_alloc ){
      idx = cur->n_row;
    }else if( i_score<i_worst ){
      idx = idx_worst;
      dbsql_free(cur->a[idx].word);
    }else{
      continue;
    }

    cur->a[idx].word = dbsql_mprintf("%s", dbsql_column_text(stmt, 1));
    if( cur->a[idx].word==0 ){
      p->rc = DBSQL_NOMEM;
      break;
    }
    cur->a[idx].i_rowid = dbsql_column_int64(stmt, 0);
    cur->a[idx].i_rank = i_rank;
    cur->a[idx].i_distance = i_dist;
    cur->a[idx].i_score = i_score;
    cur->a[idx].i_matchlen = i_matchlen;
    memcpy(cur->a[idx].hash, hash1, i_scope+1);
    if( cur->n_row<cur->n_alloc ) cur->n_row++;
    if( cur->n_row==cur->n_alloc ){
      i_worst = cur->a[0].i_score;
      idx_worst = 0;
      for(i=1; i<cur->n_row; i++){
        i_score = cur->a[i].i_score;
        if( i_worst<i_score ){
          i_worst = i_score;
          idx_worst = i;
        }
      }
    }
  }
  rc = dbsql_reset(stmt);
  if( rc ) p->rc = rc;
}

/*
** This version of the filter method work if the MATCH term is present
** and we are doing a scan.
*/
static int __spellfix1_filter_for_match(
  spellfix1_cursor *cur,
  int argc,
  dbsql_value_t **argv
){
  int idx_num = cur->idx_num;
  const unsigned char *match_this;   /* RHS of the MATCH operator */
  edit_dist3_from_string_t *match_str3 = 0; /* match_this as an editdist string */
  char *pattern;                    /* transliteration_t of match_this */
  int n_pattern;                      /* Length of pattern */
  int i_limit = 20;                   /* Max number of rows of output */
  int i_scope = 3;                    /* Use this many characters of z_class */
  int i_lang = 0;                     /* Language code */
  char *sql;                        /* SQL of shadow table query */
  dbsql_stmt_t *stmt = 0;           /* Shadow table query */
  int rc;                            /* Result code */
  int idx = 1;                       /* Next available filter parameter */
  spellfix1_vtab *p = cur->v_tab;   /* The virtual table that owns cur */
  match_query_t x;                      /* For passing info to __run_query() */

  /* Load the cost table if we have not already done so */
  if( p->cost_table!=0 && p->config3==0 ){
    p->config3 = dbsql_malloc64( sizeof(p->config3[0]) );
    if( p->config3==0 ) return DBSQL_NOMEM;
    memset(p->config3, 0, sizeof(p->config3[0]));
    rc = __edit_dist3_config_load(p->config3, p->db, p->cost_table);
    if( rc ) return rc;
  }
  memset(&x, 0, sizeof(x));
  x.i_scope = 3;  /* Default scope if none specified by "WHERE scope=N" */
  x.i_max_dist = -1;   /* Maximum allowed edit distance */

  if( idx_num&2 ){
    i_lang = dbsql_value_int(argv[idx++]);
  }
  if( idx_num&4 ){
    i_limit = dbsql_value_int(argv[idx++]);
    if( i_limit<1 ) i_limit = 1;
  }
  if( idx_num&8 ){
    x.i_scope = dbsql_value_int(argv[idx++]);
    if( x.i_scope<1 ) x.i_scope = 1;
    if( x.i_scope>SPELLFIX_MX_HASH-2 ) x.i_scope = SPELLFIX_MX_HASH-2;
  }
  if( idx_num&(16|32) ){
    x.i_max_dist = dbsql_value_int(argv[idx++]);
    if( idx_num&16 ) x.i_max_dist--;
    if( x.i_max_dist<0 ) x.i_max_dist = 0;
  }
  __spellfix1_reset_cursor(cur);
  __spellfix1_resize_cursor(cur, i_limit);
  match_this = dbsql_value_text(argv[0]);
  if( match_this==0 ) return DBSQL_OK;
  if( p->config3 ){
    x.lang = __edit_dist3_find_lang(p->config3, i_lang);
    match_str3 = __edit_dist3_from_string_new(x.lang, (const char*)match_this, -1);
    if( match_str3==0 ){
      x.rc = DBSQL_NOMEM;
      goto filter_exit;
    }
  }else{
    x.lang = 0;
  }
  pattern = (char*)transliterate(match_this, dbsql_value_bytes(argv[0]));
  dbsql_free(cur->pattern);
  cur->pattern = pattern;
  if( pattern==0 ){
    x.rc = DBSQL_NOMEM;
    goto filter_exit;
  }
  n_pattern = (int)strlen(pattern);
  if( n_pattern>0 && pattern[n_pattern-1]=='*' ) n_pattern--;
  sql = dbsql_mprintf(
     "SELECT id, word, rank, coalesce(k1,word)"
     "  FROM \"%w\".\"%w_vocab\""
     " WHERE langid=%d AND k2>=?1 AND k2<?2",
     p->db_name, p->table_name, i_lang
  );
  if( sql==0 ){
    x.rc = DBSQL_NOMEM;
    stmt = 0;
    goto filter_exit;
  }
  rc = dbsql_prepare_v2(p->db, sql, -1, &stmt, 0);
  dbsql_free(sql);
  cur->i_lang = i_lang;
  x.cur = cur;
  x.stmt = stmt;
  x.pattern = pattern;
  x.n_pattern = n_pattern;
  x.match_str3 = match_str3;
  x.i_lang = i_lang;
  x.rc = rc;
  x.config3 = p->config3;
  if( x.rc==DBSQL_OK ){
    __spellfix1_run_query(&x, pattern, n_pattern);
  }

  if( cur->a ){
    qsort(cur->a, cur->n_row, sizeof(cur->a[0]), __spellfix1_row_compare);
    cur->i_top = i_limit;
    cur->i_scope = i_scope;
  }else{
    x.rc = DBSQL_NOMEM;
  }

filter_exit:
  dbsql_finalize(stmt);
  __edit_dist3_from_string_delete(match_str3);
  return x.rc;
}

/*
** This version of filter handles a full-table scan case
*/
static int __spellfix1_filter_for_full_scan(
  spellfix1_cursor *cur,
  int argc,
  dbsql_value_t **argv
){
  int rc = DBSQL_OK;
  int idx_num = cur->idx_num;
  char *sql;
  spellfix1_vtab *v_tab = cur->v_tab;
  __spellfix1_reset_cursor(cur);
  assert( idx_num==0 || idx_num==64 );
  sql = dbsql_mprintf(
     "SELECT word, rank, NULL, langid, id FROM \"%w\".\"%w_vocab\"%s",
     v_tab->db_name, v_tab->table_name,
     ((idx_num & 64) ? " WHERE rowid=?" : "")
  );
  if( sql==0 ) return DBSQL_NOMEM;
  rc = dbsql_prepare_v2(v_tab->db, sql, -1, &cur->full_scan, 0);
  dbsql_free(sql);
  if( rc==DBSQL_OK && (idx_num & 64) ){
    assert( argc==1 );
    rc = dbsql_bind_value(cur->full_scan, 1, argv[0]);
  }
  cur->n_row = cur->i_row = 0;
  if( rc==DBSQL_OK ){
    rc = dbsql_step(cur->full_scan);
    if( rc==DBSQL_ROW ){ cur->i_row = -1; rc = DBSQL_OK; }
    if( rc==DBSQL_DONE ){ rc = DBSQL_OK; }
  }else{
    cur->i_row = 0;
  }
  return rc;
}


/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any spellfix1Column, spellfix1Rowid, or spellfix1Eof call.
*/
static int spellfix1Filter(
  dbsql_vtab_cursor_t *cur, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  spellfix1_cursor *p_cur = (spellfix1_cursor *)cur;
  int rc;
  p_cur->idx_num = idx_num;
  if( idx_num & 1 ){
    rc = __spellfix1_filter_for_match(p_cur, argc, argv);
  }else{
    rc = __spellfix1_filter_for_full_scan(p_cur, argc, argv);
  }
  return rc;
}


/*
** Advance a cursor to its next row of output
*/
static int spellfix1Next(dbsql_vtab_cursor_t *cur){
  spellfix1_cursor *p_cur = (spellfix1_cursor *)cur;
  int rc = DBSQL_OK;
  if( p_cur->i_row < p_cur->n_row ){
    if( p_cur->full_scan ){
      rc = dbsql_step(p_cur->full_scan);
      if( rc!=DBSQL_ROW ) p_cur->i_row = p_cur->n_row;
      if( rc==DBSQL_ROW || rc==DBSQL_DONE ) rc = DBSQL_OK;
    }else{
      p_cur->i_row++;
    }
  }
  return rc;
}

/*
** Return TRUE if we are at the end-of-file
*/
static int spellfix1Eof(dbsql_vtab_cursor_t *cur){
  spellfix1_cursor *p_cur = (spellfix1_cursor *)cur;
  return p_cur->i_row>=p_cur->n_row;
}

/*
** Return columns from the current row.
*/
static int spellfix1Column(
  dbsql_vtab_cursor_t *cur,
  dbsql_func_t *ctx,
  int i
){
  spellfix1_cursor *p_cur = (spellfix1_cursor*)cur;
  if( p_cur->full_scan ){
    if( i<=SPELLFIX_COL_LANGID ){
      dbsql_result_value(ctx, dbsql_column_value(p_cur->full_scan, i));
    }else{
      dbsql_result_null(ctx);
    }
    return DBSQL_OK;
  }
  switch( i ){
    case SPELLFIX_COL_WORD: {
      dbsql_result_text(ctx, p_cur->a[p_cur->i_row].word, -1, DBSQL_STATIC);
      break;
    }
    case SPELLFIX_COL_RANK: {
      dbsql_result_int(ctx, p_cur->a[p_cur->i_row].i_rank);
      break;
    }
    case SPELLFIX_COL_DISTANCE: {
      dbsql_result_int(ctx, p_cur->a[p_cur->i_row].i_distance);
      break;
    }
    case SPELLFIX_COL_LANGID: {
      dbsql_result_int(ctx, p_cur->i_lang);
      break;
    }
    case SPELLFIX_COL_SCORE: {
      dbsql_result_int(ctx, p_cur->a[p_cur->i_row].i_score);
      break;
    }
    case SPELLFIX_COL_MATCHLEN: {
      int i_matchlen = p_cur->a[p_cur->i_row].i_matchlen;
      if( i_matchlen<0 ){
        int n_pattern = (int)strlen(p_cur->pattern);
        char *word = p_cur->a[p_cur->i_row].word;
        int n_word = (int)strlen(word);

        if( n_pattern>0 && p_cur->pattern[n_pattern-1]=='*' ){
          char *z_translit;
          int res;
          z_translit = (char *)transliterate((unsigned char *)word, n_word);
          if( !z_translit ) return DBSQL_NOMEM;
          res = editdist1(p_cur->pattern, z_translit, &i_matchlen);
          dbsql_free(z_translit);
          if( res<0 ) return res==(-4) ? DBSQL_TOOBIG : DBSQL_NOMEM;
          i_matchlen = translen_to_charlen(word, n_word, i_matchlen);
        }else{
          i_matchlen = utf8Charlen(word, n_word);
        }
      }

      dbsql_result_int(ctx, i_matchlen);
      break;
    }
    case SPELLFIX_COL_PHONEHASH: {
      dbsql_result_text(ctx, p_cur->a[p_cur->i_row].hash, -1, DBSQL_STATIC);
      break;
    }
    case SPELLFIX_COL_TOP: {
      dbsql_result_int(ctx, p_cur->i_top);
      break;
    }
    case SPELLFIX_COL_SCOPE: {
      dbsql_result_int(ctx, p_cur->i_scope);
      break;
    }
    case SPELLFIX_COL_SRCHCNT: {
      dbsql_result_int(ctx, p_cur->n_search);
      break;
    }
    default: {
      dbsql_result_null(ctx);
      break;
    }
  }
  return DBSQL_OK;
}

/*
** The rowid.
*/
static int spellfix1Rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  spellfix1_cursor *p_cur = (spellfix1_cursor*)cur;
  if( p_cur->full_scan ){
    *rowid = dbsql_column_int64(p_cur->full_scan, 4);
  }else{
    *rowid = p_cur->a[p_cur->i_row].i_rowid;
  }
  return DBSQL_OK;
}

/*
** This function is called by the update() method. It returns a string
** containing the conflict mode that update() should use for the current
** operation. One of: "ROLLBACK", "IGNORE", "ABORT" or "REPLACE".
*/
static const char *__spellfix1_get_conflict(DBSQL *db){
  static const char *conflicts[] = {
    /* Note: Instead of "FAIL" - "ABORT". */
    "ROLLBACK", "IGNORE", "ABORT", "ABORT", "REPLACE"
  };
  int conflict = dbsql_vtab_on_conflict(db);

  assert( conflict==DBSQL_ROLLBACK || conflict==DBSQL_IGNORE
       || conflict==DBSQL_FAIL || conflict==DBSQL_ABORT
       || conflict==DBSQL_REPLACE
  );
  assert( DBSQL_ROLLBACK==1 );
  assert( DBSQL_IGNORE==2 );
  assert( DBSQL_FAIL==3 );
  assert( DBSQL_ABORT==4 );
  assert( DBSQL_REPLACE==5 );

  return conflicts[conflict-1];
}

/*
** The update() method.
*/
static int spellfix1Update(
  dbsql_vtab_t *v_tab,
  int argc,
  dbsql_value_t **argv,
  int64_t *p_rowid
){
  int rc = DBSQL_OK;
  int64_t rowid, new_rowid;
  spellfix1_vtab *p = (spellfix1_vtab*)v_tab;
  DBSQL *db = p->db;

  if( argc==1 ){
    /* A delete operation on the rowid given by argv[0] */
    rowid = *p_rowid = dbsql_value_int64(argv[0]);
    __spellfix1_db_exec(&rc, db, "DELETE FROM \"%w\".\"%w_vocab\" "
                           " WHERE id=%lld",
                  p->db_name, p->table_name, rowid);
  }else{
    const unsigned char *word = dbsql_value_text(argv[SPELLFIX_COL_WORD+2]);
    int n_word = dbsql_value_bytes(argv[SPELLFIX_COL_WORD+2]);
    int i_lang = dbsql_value_int(argv[SPELLFIX_COL_LANGID+2]);
    int i_rank = dbsql_value_int(argv[SPELLFIX_COL_RANK+2]);
    const unsigned char *soundslike =
           dbsql_value_text(argv[SPELLFIX_COL_SOUNDSLIKE+2]);
    int n_soundslike = dbsql_value_bytes(argv[SPELLFIX_COL_SOUNDSLIKE+2]);
    char *k1, *k2;
    int i;
    char c;
    const char *conflict = __spellfix1_get_conflict(db);

    if( word==0 ){
      /* Inserts of the form:  INSERT INTO table(command) VALUES('xyzzy');
      ** cause word to be NULL, so we look at the "command" column to see
      ** what special actions to take */
      const char *cmd = 
         (const char*)dbsql_value_text(argv[SPELLFIX_COL_COMMAND+2]);
      if( cmd==0 ){
        v_tab->errmsg = dbsql_mprintf("NOT NULL constraint failed: %s.word",
                                         p->table_name);
        return DBSQL_CONSTRAINT_NOTNULL;
      }
      if( strcmp(cmd,"reset")==0 ){
        /* Reset the  edit cost table (if there is one). */
        __edit_dist3_config_delete(p->config3);
        p->config3 = 0;
        return DBSQL_OK;
      }
      if( strncmp(cmd,"edit_cost_table=",16)==0 ){
        __edit_dist3_config_delete(p->config3);
        p->config3 = 0;
        dbsql_free(p->cost_table);
        p->cost_table = spellfix1Dequote(cmd+16);
        if( p->cost_table==0 ) return DBSQL_NOMEM;
        if( p->cost_table[0]==0 || dbsql_stricmp(p->cost_table,"null")==0 ){
          dbsql_free(p->cost_table);
          p->cost_table = 0;
        }
        return DBSQL_OK;
      }
      v_tab->errmsg = dbsql_mprintf("unknown value for %s.command: \"%w\"",
                                       p->table_name, cmd);
      return DBSQL_ERROR;
    }
    if( i_rank<1 ) i_rank = 1;
    if( soundslike ){
      k1 = (char*)transliterate(soundslike, n_soundslike);
    }else{
      k1 = (char*)transliterate(word, n_word);
    }
    if( k1==0 ) return DBSQL_NOMEM;
    for(i=0; (c = k1[i])!=0; i++){
       if( c>='A' && c<='Z' ) k1[i] += 'a' - 'A';
    }
    k2 = (char*)__phonetic_hash((const unsigned char*)k1, i);
    if( k2==0 ){
      dbsql_free(k1);
      return DBSQL_NOMEM;
    }
    if( dbsql_value_type(argv[0])==DBSQL_NULL ){
      if( dbsql_value_type(argv[1])==DBSQL_NULL ){
        __spellfix1_db_exec(&rc, db,
               "INSERT INTO \"%w\".\"%w_vocab\"(rank,langid,word,k1,k2) "
               "VALUES(%d,%d,%Q,nullif(%Q,%Q),%Q)",
               p->db_name, p->table_name,
               i_rank, i_lang, word, k1, word, k2
        );
      }else{
        new_rowid = dbsql_value_int64(argv[1]);
        __spellfix1_db_exec(&rc, db,
            "INSERT OR %s INTO \"%w\".\"%w_vocab\"(id,rank,langid,word,k1,k2) "
            "VALUES(%lld,%d,%d,%Q,nullif(%Q,%Q),%Q)",
            conflict, p->db_name, p->table_name,
            new_rowid, i_rank, i_lang, word, k1, word, k2
        );
      }
      *p_rowid = dbsql_last_insert_rowid(db);
    }else{
      rowid = dbsql_value_int64(argv[0]);
      new_rowid = *p_rowid = dbsql_value_int64(argv[1]);
      __spellfix1_db_exec(&rc, db,
             "UPDATE OR %s \"%w\".\"%w_vocab\" SET id=%lld, rank=%d, langid=%d,"
             " word=%Q, k1=nullif(%Q,%Q), k2=%Q WHERE id=%lld",
             conflict, p->db_name, p->table_name, new_rowid, i_rank, i_lang,
             word, k1, word, k2, rowid
      );
    }
    dbsql_free(k1);
    dbsql_free(k2);
  }
  return rc;
}

/*
** Rename the spellfix1 table.
*/
static int spellfix1Rename(dbsql_vtab_t *v_tab, const char *z_new){
  spellfix1_vtab *p = (spellfix1_vtab*)v_tab;
  DBSQL *db = p->db;
  int rc = DBSQL_OK;
  char *new_name = dbsql_mprintf("%s", z_new);
  if( new_name==0 ){
    return DBSQL_NOMEM;
  }
  __spellfix1_db_exec(&rc, db, 
     "ALTER TABLE \"%w\".\"%w_vocab\" RENAME TO \"%w_vocab\"",
     p->db_name, p->table_name, new_name
  );
  if( rc==DBSQL_OK ){
    dbsql_free(p->table_name);
    p->table_name = new_name;
  }else{
    dbsql_free(new_name);
  }
  return rc;
}


/*
** A virtual table module that provides fuzzy search.
*/
static dbsql_module_t spellfix1Module = {
  0,                       /* i_version */
  spellfix1Create,         /* create - handle CREATE VIRTUAL TABLE */
  spellfix1Connect,        /* connect - reconnected to an existing table */
  __spellfix1_best_index,      /* best_index - figure out how to do a query */
  spellfix1Disconnect,     /* disconnect - close a connection */
  spellfix1Destroy,        /* destroy - handle DROP TABLE */
  spellfix1Open,           /* x_open - open a cursor */
  spellfix1Close,          /* x_close - close a cursor */
  spellfix1Filter,         /* filter - configure scan constraints */
  spellfix1Next,           /* next - advance a cursor */
  spellfix1Eof,            /* eof - check for end of scan */
  spellfix1Column,         /* column - read data */
  spellfix1Rowid,          /* rowid - read data */
  spellfix1Update,         /* update */
  0,                       /* begin */
  0,                       /* x_sync */
  0,                       /* commit */
  0,                       /* rollback */
  0,                       /* find_method */
  spellfix1Rename,         /* rename */
  0,                       /* savepoint */
  0,                       /* release */
  0,                       /* rollback_to */
  0,                       /* shadow_name */
  0                        /* integrity */
};

/*
** Register the various functions and the virtual table.
*/
static int spellfix1Register(DBSQL *db){
  int rc = DBSQL_OK;
  unsigned int i;
  rc = dbsql_create_function(db, "spellfix1_translit", 1,
                               DBSQL_UTF8|DBSQL_DETERMINISTIC, 0,
                                __transliterate_sql_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "spellfix1_editdist", 2,
                                 DBSQL_UTF8|DBSQL_DETERMINISTIC, 0,
                                  __editdist_sql_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "spellfix1_phonehash", 1,
                                 DBSQL_UTF8|DBSQL_DETERMINISTIC, 0,
                                  __phonetic_hash_sql_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "spellfix1_scriptcode", 1,
                                  DBSQL_UTF8|DBSQL_DETERMINISTIC, 0,
                                  __script_code_sql_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_module(db, "spellfix1", &spellfix1Module, 0);
  }
  if( rc==DBSQL_OK ){
    rc = __edit_dist3_install(db);
  }

  /* Verify sanity of the translit[] table */
  for(i=0; i<sizeof(translit)/sizeof(translit[0])-1; i++){
    assert( translit[i].c_from<translit[i+1].c_from );
  }

  return rc;
}

#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** Extension load function.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_spellfix_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  return spellfix1Register(db);
#endif
  return DBSQL_OK;
}
