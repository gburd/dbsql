/*
** 2012-11-13
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
** The code in this file implements a compact but reasonably
** efficient regular-expression matcher for posix extended regular
** expressions against UTF8 text.
**
** This file is an dbsql extension.  It registers a single function
** named "regexp(A,B)" where A is the regular expression and B is the
** string to be matched.  By registering this function, dbsql will also
** then implement the "B regexp A" operator.  Note that with the function
** the regular expression comes first, but with the operator it comes
** second.
**
**  The following regular expression syntax is supported:
**
**     X*      zero or more occurrences of X
**     X+      one or more occurrences of X
**     X?      zero or one occurrences of X
**     X{p,q}  between p and q occurrences of X
**     (X)     match X
**     X|Y     X or Y
**     ^X      X occurring at the beginning of the string
**     X$      X occurring at the end of the string
**     .       Match any single character
**     \c      Character c where c is one of \{}()[]|*+?-.
**     \c      C-language escapes for c in afnrtv.  ex: \t or \n
**     \u_xxxx  Where XXXX is exactly 4 hex digits, unicode value XXXX
**     \xx    Where XX is exactly 2 hex digits, unicode value XX
**     [abc]   Any single character from the set abc
**     [^abc]  Any single character not in the set abc
**     [a-z]   Any single character in the range a-z
**     [^a-z]  Any single character not in the range a-z
**     \b      Word boundary
**     \w      Word character.  [A-Za-z0-9_]
**     \W      Non-word character
**     \d      Digit
**     \D      Non-digit
**     \s      Whitespace character
**     \S      Non-whitespace character
**
** A nondeterministic finite automaton (NFA) is used for matching, so the
** performance is bounded by O(N*M) where N is the size of the regular
** expression and M is the size of the input string.  The matcher never
** exhibits exponential behavior.  Note that the X{p,q} operator expands
** to p copies of X following by q-p copies of X? and that the size of the
** regular expression in the O(N*M) performance bound is computed after
** this expansion.
**
** To help prevent __do_s attacks, the maximum size of the NFA is restricted.
*/
#include <string.h>
#include <stdlib.h>
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1

/*
** The following #defines change the names of some functions implemented in
** this file to prevent name collisions with C-library functions of the
** same name.
*/
#define re_match   __dbsqlre_match
#define re_compile __dbsqlre_compile
#define re_free    __dbsqlre_free

/* The end-of-input character */
#define RE_EOF            0    /* End of input */
#define RE_START  0xfffffff    /* Start of input - larger than an UTF-8 */

/* The NFA is implemented as sequence of opcodes taken from the following
** set.  Each opcode has a single integer argument.
*/
#define RE_OP_MATCH       1    /* Match the one character in the argument */
#define RE_OP_ANY         2    /* Match any one character.  (Implements ".") */
#define RE_OP_ANYSTAR     3    /* Special optimized version of .* */
#define RE_OP_FORK        4    /* Continue to both next and opcode at i_arg */
#define RE_OP_GOTO        5    /* Jump to opcode at i_arg */
#define RE_OP_ACCEPT      6    /* Halt and indicate a successful match */
#define RE_OP_CC_INC      7    /* Beginning of a [...] character class */
#define RE_OP_CC_EXC      8    /* Beginning of a [^...] character class */
#define RE_OP_CC_VALUE    9    /* Single value in a character class */
#define RE_OP_CC_RANGE   10    /* Range of values in a character class */
#define RE_OP_WORD       11    /* Perl word character [A-Za-z0-9_] */
#define RE_OP_NOTWORD    12    /* Not a perl word character */
#define RE_OP_DIGIT      13    /* digit:  [0-9] */
#define RE_OP_NOTDIGIT   14    /* Not a digit */
#define RE_OP_SPACE      15    /* space:  [ \t\n\r\v\f] */
#define RE_OP_NOTSPACE   16    /* Not a digit */
#define RE_OP_BOUNDARY   17    /* Boundary between word and non-word */
#define RE_OP_ATSTART    18    /* Currently at the start of the string */

/* Each opcode is a "state" in the NFA */
typedef unsigned short __re_state_number;

/* Because this is an NFA and not a DFA, multiple states can be active at
** once.  An instance of the following object records all active states in
** the NFA.  The implementation is optimized for the common case where the
** number of actives states is small.
*/
typedef struct re_state_set_t {
  unsigned n_state;            /* Number of current states */
  __re_state_number *states;      /* Current states */
} re_state_set_t;

/* An input string read one character at a time.
*/
typedef struct re_input_t re_input_t;
struct re_input_t {
  const unsigned char *z;  /* All text */
  int i;                   /* Next byte to read */
  int mx;                  /* EOF when i>=mx */
};

/* A compiled NFA (or an NFA that is in the process of being compiled) is
** an instance of the following object.
*/
typedef struct re_compiled_t re_compiled_t;
struct re_compiled_t {
  re_input_t s_in;                /* Regular expression text */
  const char *err;           /* Error message to return */
  char *ops;                  /* Operators for the virtual machine */
  int *args;                  /* Arguments to each operator */
  unsigned (*next_char)(re_input_t*);  /* Next character function */
  unsigned char init[12];    /* Initial text to match */
  int n_init;                  /* Number of bytes in init */
  unsigned n_state;            /* Number of entries in ops[] and args[] */
  unsigned n_alloc;            /* Slots allocated for ops[] and args[] */
  unsigned mx_alloc;           /* Complexity limit */
};

/* Add a state to the given state set if it is not already there */
static void re_add_state(re_state_set_t *set, int new_state){
  unsigned i;
  for(i=0; i<set->n_state; i++) if( set->states[i]==new_state ) return;
  set->states[set->n_state++] = (__re_state_number)new_state;
}

/* Extract the next unicode character from *in and return it.  Advance
** *in to the first byte past the end of the character returned.  To
** be clear:  this routine converts utf8 to unicode.  This routine is 
** optimized for the common case where the next character is a single byte.
*/
static unsigned re_next_char(re_input_t *p){
  unsigned c;
  if( p->i>=p->mx ) return 0;
  c = p->z[p->i++];
  if( c>=0x80 ){
    if( (c&0xe0)==0xc0 && p->i<p->mx && (p->z[p->i]&0xc0)==0x80 ){
      c = (c&0x1f)<<6 | (p->z[p->i++]&0x3f);
      if( c<0x80 ) c = 0xfffd;
    }else if( (c&0xf0)==0xe0 && p->i+1<p->mx && (p->z[p->i]&0xc0)==0x80
           && (p->z[p->i+1]&0xc0)==0x80 ){
      c = (c&0x0f)<<12 | ((p->z[p->i]&0x3f)<<6) | (p->z[p->i+1]&0x3f);
      p->i += 2;
      if( c<=0x7ff || (c>=0xd800 && c<=0xdfff) ) c = 0xfffd;
    }else if( (c&0xf8)==0xf0 && p->i+2<p->mx && (p->z[p->i]&0xc0)==0x80
           && (p->z[p->i+1]&0xc0)==0x80 && (p->z[p->i+2]&0xc0)==0x80 ){
      c = (c&0x07)<<18 | ((p->z[p->i]&0x3f)<<12) | ((p->z[p->i+1]&0x3f)<<6)
                       | (p->z[p->i+2]&0x3f);
      p->i += 3;
      if( c<=0xffff || c>0x10ffff ) c = 0xfffd;
    }else{
      c = 0xfffd;
    }
  }
  return c;
}
static unsigned re_next_char_nocase(re_input_t *p){
  unsigned c = re_next_char(p);
  if( c>='A' && c<='Z' ) c += 'a' - 'A';
  return c;
}

/* Return true if c is a perl "word" character:  [A-Za-z0-9_] */
static int re_word_char(int c){
  return (c>='0' && c<='9') || (c>='a' && c<='z')
      || (c>='A' && c<='Z') || c=='_';
}

/* Return true if c is a "digit" character:  [0-9] */
static int re_digit_char(int c){
  return (c>='0' && c<='9');
}

/* Return true if c is a perl "space" character:  [ \t\r\n\v\f] */
static int re_space_char(int c){
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\v' || c=='\f';
}

/* Run a compiled regular expression on the zero-terminated input
** string in[].  Return true on a match and false if there is no match.
*/
static int re_match(re_compiled_t *re, const unsigned char *z_in, int n_in){
  re_state_set_t state_sets[2], *p_this, *next;
  __re_state_number spaces[100];
  __re_state_number *to_free;
  unsigned int i = 0;
  unsigned int i_swap = 0;
  int c = RE_START;
  int c_prev = 0;
  int rc = 0;
  re_input_t in;

  in.z = z_in;
  in.i = 0;
  in.mx = n_in>=0 ? n_in : (int)strlen((char const*)z_in);

  /* Look for the initial prefix match, if there is one. */
  if( re->n_init ){
    unsigned char x = re->init[0];
    while( in.i+re->n_init<=in.mx 
     && (z_in[in.i]!=x ||
         strncmp((const char*)z_in+in.i, (const char*)re->init, re->n_init)!=0)
    ){
      in.i++;
    }
    if( in.i+re->n_init>in.mx ) return 0;
    c = RE_START-1;
  }

  if( re->n_state<=(sizeof(spaces)/(sizeof(spaces[0])*2)) ){
    to_free = 0;
    state_sets[0].states = spaces;
  }else{
    to_free = dbsql_malloc64( sizeof(__re_state_number)*2*re->n_state );
    if( to_free==0 ) return -1;
    state_sets[0].states = to_free;
  }
  state_sets[1].states = &state_sets[0].states[re->n_state];
  next = &state_sets[1];
  next->n_state = 0;
  re_add_state(next, 0);
  while( c!=RE_EOF && next->n_state>0 ){
    c_prev = c;
    c = re->next_char(&in);
    p_this = next;
    next = &state_sets[i_swap];
    i_swap = 1 - i_swap;
    next->n_state = 0;
    for(i=0; i<p_this->n_state; i++){
      int x = p_this->states[i];
      switch( re->ops[x] ){
        case RE_OP_MATCH: {
          if( re->args[x]==c ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_ATSTART: {
          if( c_prev==RE_START ) re_add_state(p_this, x+1);
          break;
        }
        case RE_OP_ANY: {
          if( c!=0 ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_WORD: {
          if( re_word_char(c) ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_NOTWORD: {
          if( !re_word_char(c) && c!=0 ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_DIGIT: {
          if( re_digit_char(c) ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_NOTDIGIT: {
          if( !re_digit_char(c) && c!=0 ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_SPACE: {
          if( re_space_char(c) ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_NOTSPACE: {
          if( !re_space_char(c) && c!=0 ) re_add_state(next, x+1);
          break;
        }
        case RE_OP_BOUNDARY: {
          if( re_word_char(c)!=re_word_char(c_prev) ) re_add_state(p_this, x+1);
          break;
        }
        case RE_OP_ANYSTAR: {
          re_add_state(next, x);
          re_add_state(p_this, x+1);
          break;
        }
        case RE_OP_FORK: {
          re_add_state(p_this, x+re->args[x]);
          re_add_state(p_this, x+1);
          break;
        }
        case RE_OP_GOTO: {
          re_add_state(p_this, x+re->args[x]);
          break;
        }
        case RE_OP_ACCEPT: {
          rc = 1;
          goto re_match_end;
        }
        case RE_OP_CC_EXC: {
          if( c==0 ) break;
          /* fall-through */ goto re_op_cc_inc;
        }
        case RE_OP_CC_INC: re_op_cc_inc: {
          int j = 1;
          int n = re->args[x];
          int hit = 0;
          for(j=1; j>0 && j<n; j++){
            if( re->ops[x+j]==RE_OP_CC_VALUE ){
              if( re->args[x+j]==c ){
                hit = 1;
                j = -1;
              }
            }else{
              if( re->args[x+j]<=c && re->args[x+j+1]>=c ){
                hit = 1;
                j = -1;
              }else{
                j++;
              }
            }
          }
          if( re->ops[x]==RE_OP_CC_EXC ) hit = !hit;
          if( hit ) re_add_state(next, x+n);
          break;
        }
      }
    }
  }
  for(i=0; i<next->n_state; i++){
    int x = next->states[i];
    while( re->ops[x]==RE_OP_GOTO ) x += re->args[x];
    if( re->ops[x]==RE_OP_ACCEPT ){ rc = 1; break; }
  }
re_match_end:
  dbsql_free(to_free);
  return rc;
}

/* Resize the opcode and argument arrays for an RE under construction.
*/
static int re_resize(re_compiled_t *p, unsigned int N){
  char *ops;
  int *args;
  if( N>p->mx_alloc ){ p->err = "REGEXP pattern too big"; return 1; }
  ops = dbsql_realloc64(p->ops, N*sizeof(p->ops[0]));
  if( ops==0 ){ p->err = "out of memory"; return 1; }
  p->ops = ops;
  args = dbsql_realloc64(p->args, N*sizeof(p->args[0]));
  if( args==0 ){ p->err = "out of memory"; return 1; }
  p->args = args;
  p->n_alloc = N;
  return 0;
}

/* Insert a new opcode and argument into an RE under construction.  The
** insertion point is just prior to existing opcode i_before.
*/
static int re_insert(re_compiled_t *p, int i_before, int op, int arg){
  int i;
  if( p->n_alloc<=p->n_state && re_resize(p, p->n_alloc*2) ) return 0;
  for(i=p->n_state; i>i_before; i--){
    p->ops[i] = p->ops[i-1];
    p->args[i] = p->args[i-1];
  }
  p->n_state++;
  p->ops[i_before] = (char)op;
  p->args[i_before] = arg;
  return i_before;
}

/* Append a new opcode and argument to the end of the RE under construction.
*/
static int re_append(re_compiled_t *p, int op, int arg){
  return re_insert(p, p->n_state, op, arg);
}

/* Make a copy of N opcodes starting at i_start onto the end of the RE
** under construction.
*/
static void re_copy(re_compiled_t *p, int i_start, unsigned int N){
  if( p->n_state+N>=p->n_alloc && re_resize(p, p->n_alloc*2+N) ) return;
  memcpy(&p->ops[p->n_state], &p->ops[i_start], N*sizeof(p->ops[0]));
  memcpy(&p->args[p->n_state], &p->args[i_start], N*sizeof(p->args[0]));
  p->n_state += N;
}

/* Return true if c is a hexadecimal digit character:  [0-9a-f_a-F]
** If c is a hex digit, also set *v = (*v)*16 + valueof(c).  If
** c is not a hex digit *v is unchanged.
*/
static int re_hex(int c, int *v){
  if( c>='0' && c<='9' ){
    c -= '0';
  }else if( c>='a' && c<='f' ){
    c -= 'a' - 10;
  }else if( c>='A' && c<='F' ){
    c -= 'A' - 10;
  }else{
    return 0;
  }
  *v = (*v)*16 + (c & 0xff);
  return 1;
}

/* A backslash character has been seen, read the next character and
** return its interpretation.
*/
static unsigned re_esc_char(re_compiled_t *p){
  static const char esc[] = "afnrtv\\()*.+?[$^{|}]-";
  static const char trans[] = "\a\f\n\r\t\v";
  int i, v = 0;
  char c;
  if( p->s_in.i>=p->s_in.mx ) return 0;
  c = p->s_in.z[p->s_in.i];
  if( c=='u' && p->s_in.i+4<p->s_in.mx ){
    const unsigned char *in = p->s_in.z + p->s_in.i;
    if( re_hex(in[1],&v)
     && re_hex(in[2],&v)
     && re_hex(in[3],&v)
     && re_hex(in[4],&v)
    ){
      p->s_in.i += 5;
      return v;
    }
  }
  if( c=='x' && p->s_in.i+2<p->s_in.mx ){
    const unsigned char *in = p->s_in.z + p->s_in.i;
    if( re_hex(in[1],&v)
     && re_hex(in[2],&v)
    ){
      p->s_in.i += 3;
      return v;
    }
  }
  for(i=0; esc[i] && esc[i]!=c; i++){}
  if( esc[i] ){
    if( i<6 ) c = trans[i];
    p->s_in.i++;
  }else{
    p->err = "unknown \\ escape";
  }
  return c;
}

/* Forward declaration */
static const char *re_subcompile_string(re_compiled_t*);

/* Peek at the next byte of input */
static unsigned char __re_peek(re_compiled_t *p){
  return p->s_in.i<p->s_in.mx ? p->s_in.z[p->s_in.i] : 0;
}

/* Compile RE text into a sequence of opcodes.  Continue up to the
** first unmatched ")" character, then return.  If an error is found,
** return a pointer to the error message string.
*/
static const char *re_subcompile_re(re_compiled_t *p){
  const char *err;
  int i_start, i_end, i_goto;
  i_start = p->n_state;
  err = re_subcompile_string(p);
  if( err ) return err;
  while( __re_peek(p)=='|' ){
    i_end = p->n_state;
    re_insert(p, i_start, RE_OP_FORK, i_end + 2 - i_start);
    i_goto = re_append(p, RE_OP_GOTO, 0);
    p->s_in.i++;
    err = re_subcompile_string(p);
    if( err ) return err;
    p->args[i_goto] = p->n_state - i_goto;
  }
  return 0;
}

/* Compile an element of regular expression text (anything that can be
** an operand to the "|" operator).  Return NULL on success or a pointer
** to the error message if there is a problem.
*/
static const char *re_subcompile_string(re_compiled_t *p){
  int i_prev = -1;
  int i_start;
  unsigned c;
  const char *err;
  while( (c = p->next_char(&p->s_in))!=0 ){
    i_start = p->n_state;
    switch( c ){
      case '|':
      case ')': {
        p->s_in.i--;
        return 0;
      }
      case '(': {
        err = re_subcompile_re(p);
        if( err ) return err;
        if( __re_peek(p)!=')' ) return "unmatched '('";
        p->s_in.i++;
        break;
      }
      case '.': {
        if( __re_peek(p)=='*' ){
          re_append(p, RE_OP_ANYSTAR, 0);
          p->s_in.i++;
        }else{
          re_append(p, RE_OP_ANY, 0);
        }
        break;
      }
      case '*': {
        if( i_prev<0 ) return "'*' without operand";
        re_insert(p, i_prev, RE_OP_GOTO, p->n_state - i_prev + 1);
        re_append(p, RE_OP_FORK, i_prev - p->n_state + 1);
        break;
      }
      case '+': {
        if( i_prev<0 ) return "'+' without operand";
        re_append(p, RE_OP_FORK, i_prev - p->n_state);
        break;
      }
      case '?': {
        if( i_prev<0 ) return "'?' without operand";
        re_insert(p, i_prev, RE_OP_FORK, p->n_state - i_prev+1);
        break;
      }
      case '$': {
        re_append(p, RE_OP_MATCH, RE_EOF);
        break;
      }
      case '^': {
        re_append(p, RE_OP_ATSTART, 0);
        break;
      }
      case '{': {
        unsigned int m = 0, n = 0;
        unsigned int sz, j;
        if( i_prev<0 ) return "'{m,n}' without operand";
        while( (c=__re_peek(p))>='0' && c<='9' ){
          m = m*10 + c - '0';
          if( m*2>p->mx_alloc ) return "REGEXP pattern too big";
          p->s_in.i++;
        }
        n = m;
        if( c==',' ){
          p->s_in.i++;
          n = 0;
          while( (c=__re_peek(p))>='0' && c<='9' ){
            n = n*10 + c-'0';
            if( n*2>p->mx_alloc ) return "REGEXP pattern too big";
            p->s_in.i++;
          }
        }
        if( c!='}' ) return "unmatched '{'";
        if( n<m ) return "n less than m in '{m,n}'";
        p->s_in.i++;
        sz = p->n_state - i_prev;
        if( m==0 ){
          if( n==0 ) return "both m and n are zero in '{m,n}'";
          re_insert(p, i_prev, RE_OP_FORK, sz+1);
          i_prev++;
          n--;
        }else{
          for(j=1; j<m; j++) re_copy(p, i_prev, sz);
        }
        for(j=m; j<n; j++){
          re_append(p, RE_OP_FORK, sz+1);
          re_copy(p, i_prev, sz);
        }
        if( n==0 && m>0 ){
          re_append(p, RE_OP_FORK, -(int)sz);
        }
        break;
      }
      case '[': {
        unsigned int i_first = p->n_state;
        if( __re_peek(p)=='^' ){
          re_append(p, RE_OP_CC_EXC, 0);
          p->s_in.i++;
        }else{
          re_append(p, RE_OP_CC_INC, 0);
        }
        while( (c = p->next_char(&p->s_in))!=0 ){
          if( c=='[' && __re_peek(p)==':' ){
            return "POSIX character classes not supported";
          }
          if( c=='\\' ) c = re_esc_char(p);
          if( __re_peek(p)=='-' ){
            re_append(p, RE_OP_CC_RANGE, c);
            p->s_in.i++;
            c = p->next_char(&p->s_in);
            if( c=='\\' ) c = re_esc_char(p);
            re_append(p, RE_OP_CC_RANGE, c);
          }else{
            re_append(p, RE_OP_CC_VALUE, c);
          }
          if( __re_peek(p)==']' ){ p->s_in.i++; break; }
        }
        if( c==0 ) return "unclosed '['";
        if( p->n_state>i_first ) p->args[i_first] = p->n_state - i_first;
        break;
      }
      case '\\': {
        int special_op = 0;
        switch( __re_peek(p) ){
          case 'b': special_op = RE_OP_BOUNDARY;   break;
          case 'd': special_op = RE_OP_DIGIT;      break;
          case 'D': special_op = RE_OP_NOTDIGIT;   break;
          case 's': special_op = RE_OP_SPACE;      break;
          case 'S': special_op = RE_OP_NOTSPACE;   break;
          case 'w': special_op = RE_OP_WORD;       break;
          case 'W': special_op = RE_OP_NOTWORD;    break;
        }
        if( special_op ){
          p->s_in.i++;
          re_append(p, special_op, 0);
        }else{
          c = re_esc_char(p);
          re_append(p, RE_OP_MATCH, c);
        }
        break;
      }
      default: {
        re_append(p, RE_OP_MATCH, c);
        break;
      }
    }
    i_prev = i_start;
  }
  return 0;
}

/* Free and reclaim all the memory used by a previously compiled
** regular expression.  Applications should invoke this routine once
** for every call to re_compile() to avoid memory leaks.
*/
static void re_free(re_compiled_t *re){
  if( re ){
    dbsql_free(re->ops);
    dbsql_free(re->args);
    dbsql_free(re);
  }
}

/*
** Version of re_free() that accepts a pointer of type (void*). Required
** to satisfy sanitizers when the re_free() function is called via a
** function pointer.
*/
static void re_free_voidptr(void *p){
  re_free((re_compiled_t*)p);
}

/*
** Compile a textual regular expression in in[] into a compiled regular
** expression suitable for us by re_match() and return a pointer to the
** compiled regular expression in *re.  Return NULL on success or an
** error message if something goes wrong.
*/
static const char *re_compile(
  re_compiled_t **pp_re,      /* OUT: write compiled NFA here */
  const char *in,        /* Input regular expression */
  int mx_re,               /* Complexity limit */
  int no_case              /* True for caseless comparisons */
){
  re_compiled_t *re;
  const char *err;
  int i, j;

  *pp_re = 0;
  re = dbsql_malloc64( sizeof(*re) );
  if( re==0 ){
    return "out of memory";
  }
  memset(re, 0, sizeof(*re));
  re->next_char = no_case ? re_next_char_nocase : re_next_char;
  re->mx_alloc = mx_re;
  if( re_resize(re, 30) ){
    err = re->err;
    re_free(re);
    return err;
  }
  if( in[0]=='^' ){
    in++;
  }else{
    re_append(re, RE_OP_ANYSTAR, 0);
  }
  re->s_in.z = (unsigned char*)in;
  re->s_in.i = 0;
  re->s_in.mx = (int)strlen(in);
  err = re_subcompile_re(re);
  if( err ){
    re_free(re);
    return err;
  }
  if( re->s_in.i>=re->s_in.mx ){
    re_append(re, RE_OP_ACCEPT, 0);
    *pp_re = re;
  }else{
    re_free(re);
    return "unrecognized character";
  }

  /* The following is a performance optimization.  If the regex begins with
  ** ".*" (if the input regex lacks an initial "^") and afterwards there are
  ** one or more matching characters, enter those matching characters into
  ** init[].  The re_match() routine can then search ahead in the input 
  ** string looking for the initial match without having to run the whole
  ** regex engine over the string.  Do not worry about trying to match
  ** unicode characters beyond plane 0 - those are very rare and this is
  ** just an optimization. */
  if( re->ops[0]==RE_OP_ANYSTAR && !no_case ){
    for(j=0, i=1; j<(int)sizeof(re->init)-2 && re->ops[i]==RE_OP_MATCH; i++){
      unsigned x = re->args[i];
      if( x<=0x7f ){
        re->init[j++] = (unsigned char)x;
      }else if( x<=0x7ff ){
        re->init[j++] = (unsigned char)(0xc0 | (x>>6));
        re->init[j++] = 0x80 | (x&0x3f);
      }else if( x<=0xffff ){
        re->init[j++] = (unsigned char)(0xe0 | (x>>12));
        re->init[j++] = 0x80 | ((x>>6)&0x3f);
        re->init[j++] = 0x80 | (x&0x3f);
      }else{
        break;
      }
    }
    if( j>0 && re->init[j-1]==0 ) j--;
    re->n_init = j;
  }
  return re->err;
}

/*
** The value of LIMIT_MAX_PATTERN_LENGTH.
*/
static int re_maxlen(dbsql_func_t *context){
  DBSQL *db = dbsql_context_db_handle(context);
  return dbsql_limit(db, DBSQL_LIMIT_LIKE_PATTERN_LENGTH,-1);
}

/*
** Maximum NFA size given a maximum pattern length.
*/
static int re_maxnfa(int mxlen){
  return 75+mxlen/2;
}

/*
** Implementation of the regexp() SQL function.  This function implements
** the build-in REGEXP operator.  The first argument to the function is the
** pattern and the second argument is the string.  So, the SQL statements:
**
**       A REGEXP B
**
** is implemented as regexp(B,A).
*/
static void re_sql_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  re_compiled_t *re;          /* Compiled regular expression */
  const char *pattern;     /* The regular expression */
  const unsigned char *str;/* String being searched */
  const char *err;         /* Compile error message */
  int set_aux = 0;           /* True to invoke dbsql_set_auxdata() */

  (void)argc;  /* Unused */
  re = dbsql_get_auxdata(context, 0);
  if( re==0 ){
    int mx_len = re_maxlen(context);
    int n_pattern;
    pattern = (const char*)dbsql_value_text(argv[0]);
    if( pattern==0 ) return;
    n_pattern = dbsql_value_bytes(argv[0]);
    if( n_pattern>mx_len ){
      err = "REGEXP pattern too big";
    }else{
      err = re_compile(&re, pattern, re_maxnfa(mx_len),
                        dbsql_user_data(context)!=0);
    }
    if( err ){
      re_free(re);
      dbsql_result_error(context, err, -1);
      return;
    }
    if( re==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
    set_aux = 1;
  }
  str = (const unsigned char*)dbsql_value_text(argv[1]);
  if( str!=0 ){
    dbsql_result_int(context, re_match(re, str, -1));
  }
  if( set_aux ){
    dbsql_set_auxdata(context, 0, re, re_free_voidptr);
  }
}

#if defined(DBSQL_DEBUG)
/*
** This function is used for testing and debugging only.  It is only available
** if the DBSQL_DEBUG compile-time option is used.
**
** Compile a regular expression and then convert the compiled expression into
** text and return that text.
*/
static void re_bytecode_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *pattern;
  const char *err;
  re_compiled_t *re;
  dbsql_str_t *str;
  int i;
  int n;
  char *z;
  static const char *__re_op_name[] = {
    "EOF",
    "MATCH",
    "ANY",
    "ANYSTAR",
    "FORK",
    "GOTO",
    "ACCEPT",
    "CC_INC",
    "CC_EXC",
    "CC_VALUE",
    "CC_RANGE",
    "WORD",
    "NOTWORD",
    "DIGIT",
    "NOTDIGIT",
    "SPACE",
    "NOTSPACE",
    "BOUNDARY",
    "ATSTART",
  };

  (void)argc;
  pattern = (const char*)dbsql_value_text(argv[0]);
  if( pattern==0 ) return;
  err = re_compile(&re, pattern, re_maxnfa(re_maxlen(context)),
                    dbsql_user_data(context)!=0);
  if( err ){
    re_free(re);
    dbsql_result_error(context, err, -1);
    return;
  }
  if( re==0 ){
    dbsql_result_error_nomem(context);
    return;
  }
  str = dbsql_str_new(0);
  if( str==0 ) goto re_bytecode_func_err;
  if( re->n_init>0 ){
    dbsql_str_appendf(str, "INIT     ");
    for(i=0; i<re->n_init; i++){
      dbsql_str_appendf(str, "%02x", re->init[i]);
    }
    dbsql_str_appendf(str, "\n");
  }
  for(i=0; (unsigned)i<re->n_state; i++){
    dbsql_str_appendf(str, "%-8s %4d\n",
         __re_op_name[(unsigned char)re->ops[i]], re->args[i]);
  }
  n = dbsql_str_length(str);
  z = dbsql_str_finish(str);
  if( n==0 ){
    dbsql_free(z);
  }else{
    dbsql_result_text(context, z, n-1, dbsql_free);
  }

re_bytecode_func_err:
  re_free(re);
}

#endif /* DBSQL_DEBUG */


/*
** Invoke this routine to register the regexp() function with the
** dbsql database connection.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_regexp_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused */
  rc = dbsql_create_function(db, "regexp", 2, 
                            DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                            0, re_sql_func, 0, 0);
  if( rc==DBSQL_OK ){
    /* The regexpi(PATTERN,STRING) function is a case-insensitive version
    ** of regexp(PATTERN,STRING). */
    rc = dbsql_create_function(db, "regexpi", 2,
                            DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                            (void*)db, re_sql_func, 0, 0);
#if defined(DBSQL_DEBUG)
    if( rc==DBSQL_OK ){
      rc = dbsql_create_function(db, "regexp_bytecode", 1,
                            DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                            0, re_bytecode_func, 0, 0);
    }
#endif /* DBSQL_DEBUG */
  }
  return rc;
}
