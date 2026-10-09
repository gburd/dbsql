/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
*/


#include "fts5_int.h"

/**************************************************************************
** Start of ascii tokenizer implementation.
*/

/*
** For tokenizers with no "unicode" modifier, the set of token characters
** is the same as the set of ASCII range alphanumeric characters. 
*/
static unsigned char __a_ascii_token_char[128] = {
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,   /* 0x00..0x0F */
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,   /* 0x10..0x1F */
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,   /* 0x20..0x2F */
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 0, 0, 0, 0, 0, 0,   /* 0x30..0x3F */
  0, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,   /* 0x40..0x4F */
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 0, 0,   /* 0x50..0x5F */
  0, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,   /* 0x60..0x6F */
  1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 0, 0,   /* 0x70..0x7F */
};

typedef struct ascii_tokenizer_t ascii_tokenizer_t;
struct ascii_tokenizer_t {
  unsigned char token_chars[128];
};

static void __fts5_ascii_add_exceptions(
  ascii_tokenizer_t *p, 
  const char *arg, 
  int token_chars
){
  int i;
  for(i=0; arg[i]; i++){
    if( (arg[i] & 0x80)==0 ){
      p->token_chars[(int)arg[i]] = (unsigned char)token_chars;
    }
  }
}

/*
** Delete a "ascii" tokenizer.
*/
static void __fts5_ascii_delete(fts5_tokenizer_t *p){
  dbsql_free(p);
}

/*
** Create an "ascii" tokenizer.
*/
static int __fts5_ascii_create(
  void *unused, 
  const char **args, int n_arg,
  fts5_tokenizer_t **out
){
  int rc = DBSQL_OK;
  ascii_tokenizer_t *p = 0;
  UNUSED_PARAM(unused);
  if( n_arg%2 ){
    rc = DBSQL_ERROR;
  }else{
    p = dbsql_malloc64(sizeof(ascii_tokenizer_t));
    if( p==0 ){
      rc = DBSQL_NOMEM;
    }else{
      int i;
      memset(p, 0, sizeof(ascii_tokenizer_t));
      memcpy(p->token_chars, __a_ascii_token_char, sizeof(__a_ascii_token_char));
      for(i=0; rc==DBSQL_OK && i<n_arg; i+=2){
        const char *arg = args[i+1];
        if( 0==dbsql_stricmp(args[i], "tokenchars") ){
          __fts5_ascii_add_exceptions(p, arg, 1);
        }else
        if( 0==dbsql_stricmp(args[i], "separators") ){
          __fts5_ascii_add_exceptions(p, arg, 0);
        }else{
          rc = DBSQL_ERROR;
        }
      }
      if( rc!=DBSQL_OK ){
        __fts5_ascii_delete((fts5_tokenizer_t*)p);
        p = 0;
      }
    }
  }

  *out = (fts5_tokenizer_t*)p;
  return rc;
}


static void __ascii_fold(char *outs, const char *ins, int n_byte){
  int i;
  for(i=0; i<n_byte; i++){
    char c = ins[i];
    if( c>='A' && c<='Z' ) c += 32;
    outs[i] = c;
  }
}

/*
** Tokenize some text using the ascii tokenizer.
*/
static int __fts5_ascii_tokenize(
  fts5_tokenizer_t *tokenizer,
  void *ctx,
  int i_unused,
  const char *text, int n_text,
  int (*token)(void*, int, const char*, int n_token, int i_start, int i_end)
){
  ascii_tokenizer_t *p = (ascii_tokenizer_t*)tokenizer;
  int rc = DBSQL_OK;
  int ie;
  int is = 0;

  char folds[64];
  int n_fold = sizeof(folds);
  char *fold = folds;
  unsigned char *a = p->token_chars;

  UNUSED_PARAM(i_unused);

  while( is<n_text && rc==DBSQL_OK ){
    int n_byte;

    /* Skip any leading divider characters. */
    while( is<n_text && ((text[is]&0x80)==0 && a[(int)text[is]]==0) ){
      is++;
    }
    if( is==n_text ) break;

    /* Count the token characters */
    ie = is+1;
    while( ie<n_text && ((text[ie]&0x80) || a[(int)text[ie]] ) ){
      ie++;
    }

    /* Fold to lower case */
    n_byte = ie-is;
    if( n_byte>n_fold ){
      if( fold!=folds ) dbsql_free(fold);
      fold = dbsql_malloc64((int64_t)n_byte*2);
      if( fold==0 ){
        rc = DBSQL_NOMEM;
        break;
      }
      n_fold = n_byte*2;
    }
    __ascii_fold(fold, &text[is], n_byte);

    /* Invoke the token callback */
    rc = token(ctx, 0, fold, n_byte, is, ie);
    is = ie+1;
  }
  
  if( fold!=folds ) dbsql_free(fold);
  if( rc==DBSQL_DONE ) rc = DBSQL_OK;
  return rc;
}

/**************************************************************************
** Start of unicode61 tokenizer implementation.
*/


/*
** The following two macros - READ_UTF8 and WRITE_UTF8 - have been copied
** from the DBSQL source file utf.c. If this file is compiled as part
** of the amalgamation, they are not required.
*/
#ifndef DBSQL_AMALGAMATION

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

#define READ_UTF8(in, term, c)                           \
  c = *(in++);                                            \
  if( c>=0xc0 ){                                           \
    c = __utf8_trans1[c-0xc0];                         \
    while( in<term && (*in & 0xc0)==0x80 ){             \
      c = (c<<6) + (0x3f & *(in++));                      \
    }                                                      \
    if( c<0x80                                             \
        || (c&0xFFFFF800)==0xD800                          \
        || (c&0xFFFFFFFE)==0xFFFE ){  c = 0xFFFD; }        \
  }


#define WRITE_UTF8(out, c) {                          \
  if( c<0x00080 ){                                     \
    *out++ = (unsigned char)(c&0xFF);                 \
  }                                                    \
  else if( c<0x00800 ){                                \
    *out++ = 0xC0 + (unsigned char)((c>>6)&0x1F);     \
    *out++ = 0x80 + (unsigned char)(c & 0x3F);        \
  }                                                    \
  else if( c<0x10000 ){                                \
    *out++ = 0xE0 + (unsigned char)((c>>12)&0x0F);    \
    *out++ = 0x80 + (unsigned char)((c>>6) & 0x3F);   \
    *out++ = 0x80 + (unsigned char)(c & 0x3F);        \
  }else{                                               \
    *out++ = 0xF0 + (unsigned char)((c>>18) & 0x07);  \
    *out++ = 0x80 + (unsigned char)((c>>12) & 0x3F);  \
    *out++ = 0x80 + (unsigned char)((c>>6) & 0x3F);   \
    *out++ = 0x80 + (unsigned char)(c & 0x3F);        \
  }                                                    \
}

#endif /* ifndef DBSQL_AMALGAMATION */

#define FTS5_SKIP_UTF8(in) {                               \
  if( ((unsigned char)(*(in++)))>=0xc0 ){                              \
    while( (((unsigned char)*in) & 0xc0)==0x80 ){ in++; }             \
  }                                                    \
}

typedef struct unicode61_tokenizer_t unicode61_tokenizer_t;
struct unicode61_tokenizer_t {
  unsigned char token_chars[128];  /* ASCII range token characters */
  char *folds;                    /* Buffer to fold text into */
  int n_fold;                      /* Size of folds[] in bytes */
  int remove_diacritic;           /* True if remove_diacritics=1 is set */
  int n_exception;
  int *exceptions;

  unsigned char categories[32];    /* True for token char categories */
};

/* Values for remove_diacritic (must match internals of fts5_unicode2.c) */
#define FTS5_REMOVE_DIACRITICS_NONE    0
#define FTS5_REMOVE_DIACRITICS_SIMPLE  1
#define FTS5_REMOVE_DIACRITICS_COMPLEX 2

static int __fts5_unicode_add_exceptions(
  unicode61_tokenizer_t *p,          /* Tokenizer object */
  const char *z,                  /* Characters to treat as exceptions */
  int token_chars                 /* 1 for 'tokenchars', 0 for 'separators' */
){
  int rc = DBSQL_OK;
  int n = (int)strlen(z);
  int *news;

  if( n>0 ){
    news = (int*)dbsql_realloc64(p->exceptions,
                                   (n+p->n_exception)*sizeof(int));
    if( news ){
      int n_new = p->n_exception;
      const unsigned char *csr = (const unsigned char*)z;
      const unsigned char *term = (const unsigned char*)&z[n];
      while( csr<term ){
        u_int32_t i_code;
        int token;
        READ_UTF8(csr, term, i_code);
        if( i_code<128 ){
          p->token_chars[i_code] = (unsigned char)token_chars;
        }else{
          token = p->categories[__fts5_unicode_category(i_code)];
          assert( (token==0 || token==1) ); 
          assert( (token_chars==0 || token_chars==1) );
          if( token!=token_chars && __fts5_unicode_isdiacritic(i_code)==0 ){
            int i;
            for(i=0; i<n_new; i++){
              if( (u_int32_t)news[i]>i_code ) break;
            }
            memmove(&news[i+1], &news[i], (n_new-i)*sizeof(int));
            news[i] = i_code;
            n_new++;
          }
        }
      }
      p->exceptions = news;
      p->n_exception = n_new;
    }else{
      rc = DBSQL_NOMEM;
    }
  }

  return rc;
}

/*
** Return true if the p->exceptions[] array contains the value i_code.
*/
static int __fts5_unicode_is_exception(unicode61_tokenizer_t *p, int i_code){
  if( p->n_exception>0 ){
    int *a = p->exceptions;
    int i_lo = 0;
    int i_hi = p->n_exception-1;

    while( i_hi>=i_lo ){
      int i_test = (i_hi + i_lo) / 2;
      if( i_code==a[i_test] ){
        return 1;
      }else if( i_code>a[i_test] ){
        i_lo = i_test+1;
      }else{
        i_hi = i_test-1;
      }
    }
  }

  return 0;
}

/*
** Delete a "unicode61" tokenizer.
*/
static void __fts5_unicode_delete(fts5_tokenizer_t *tok){
  if( tok ){
    unicode61_tokenizer_t *p = (unicode61_tokenizer_t*)tok;
    dbsql_free(p->exceptions);
    dbsql_free(p->folds);
    dbsql_free(p);
  }
  return;
}

static int __unicode_set_categories(unicode61_tokenizer_t *p, const char *cat){
  const char *z = cat;

  while( *z ){
    while( *z==' ' || *z=='\t' ) z++;
    if( *z && __fts5_unicode_cat_parse(z, p->categories) ){
      return DBSQL_ERROR;
    }
    while( *z!=' ' && *z!='\t' && *z!='\0' ) z++;
  }

  __fts5_unicode_ascii(p->categories, p->token_chars);
  return DBSQL_OK;
}

/*
** Create a "unicode61" tokenizer.
*/
static int __fts5_unicode_create(
  void *unused, 
  const char **args, int n_arg,
  fts5_tokenizer_t **out
){
  int rc = DBSQL_OK;             /* Return code */
  unicode61_tokenizer_t *p = 0;      /* New tokenizer object */ 

  UNUSED_PARAM(unused);

  if( n_arg%2 ){
    rc = DBSQL_ERROR;
  }else{
    p = (unicode61_tokenizer_t*)dbsql_malloc64(sizeof(unicode61_tokenizer_t));
    if( p ){
      const char *cat = "L* N* Co";
      int i;
      memset(p, 0, sizeof(unicode61_tokenizer_t));

      p->remove_diacritic = FTS5_REMOVE_DIACRITICS_SIMPLE;
      p->n_fold = 64;
      p->folds = dbsql_malloc64(p->n_fold * sizeof(char));
      if( p->folds==0 ){
        rc = DBSQL_NOMEM;
      }

      /* Search for a "categories" argument */
      for(i=0; rc==DBSQL_OK && i<n_arg; i+=2){
        if( 0==dbsql_stricmp(args[i], "categories") ){
          cat = args[i+1];
        }
      }
      if( rc==DBSQL_OK ){
        rc = __unicode_set_categories(p, cat);
      }

      for(i=0; rc==DBSQL_OK && i<n_arg; i+=2){
        const char *arg = args[i+1];
        if( 0==dbsql_stricmp(args[i], "remove_diacritics") ){
          if( (arg[0]!='0' && arg[0]!='1' && arg[0]!='2') || arg[1] ){
            rc = DBSQL_ERROR;
          }else{
            p->remove_diacritic = (arg[0] - '0');
            assert( p->remove_diacritic==FTS5_REMOVE_DIACRITICS_NONE
                 || p->remove_diacritic==FTS5_REMOVE_DIACRITICS_SIMPLE
                 || p->remove_diacritic==FTS5_REMOVE_DIACRITICS_COMPLEX
            );
          }
        }else
        if( 0==dbsql_stricmp(args[i], "tokenchars") ){
          rc = __fts5_unicode_add_exceptions(p, arg, 1);
        }else
        if( 0==dbsql_stricmp(args[i], "separators") ){
          rc = __fts5_unicode_add_exceptions(p, arg, 0);
        }else
        if( 0==dbsql_stricmp(args[i], "categories") ){
          /* no-op */
        }else{
          rc = DBSQL_ERROR;
        }
      }
    }else{
      rc = DBSQL_NOMEM;
    }
    if( rc!=DBSQL_OK ){
      __fts5_unicode_delete((fts5_tokenizer_t*)p);
      p = 0;
    }
    *out = (fts5_tokenizer_t*)p;
  }
  return rc;
}

/*
** Return true if, for the purposes of tokenizing with the tokenizer
** passed as the first argument, codepoint i_code is considered a token 
** character (not a separator).
*/
static int __fts5_unicode_is_alnum(unicode61_tokenizer_t *p, int i_code){
  return (
    p->categories[__fts5_unicode_category((u_int32_t)i_code)]
    ^ __fts5_unicode_is_exception(p, i_code)
  );
}

static int __fts5_unicode_tokenize(
  fts5_tokenizer_t *tokenizer,
  void *ctx,
  int i_unused,
  const char *text, int n_text,
  int (*token)(void*, int, const char*, int n_token, int i_start, int i_end)
){
  unicode61_tokenizer_t *p = (unicode61_tokenizer_t*)tokenizer;
  int rc = DBSQL_OK;
  unsigned char *a = p->token_chars;

  unsigned char *term = (unsigned char*)&text[n_text];
  unsigned char *csr = (unsigned char *)text;

  /* Output buffer */
  char *folds = p->folds;
  int n_fold = p->n_fold;
  const char *end = &folds[n_fold-6];

  UNUSED_PARAM(i_unused);

  /* Each iteration of this loop gobbles up a contiguous run of separators,
  ** then the next token.  */
  while( rc==DBSQL_OK ){
    u_int32_t i_code;                    /* non-ASCII codepoint read from input */
    char *out = folds;
    int is;
    int ie;

    /* Skip any separator characters. */
    while( 1 ){
      if( csr>=term ) goto tokenize_done;
      if( *csr & 0x80 ) {
        /* A character outside of the ascii range. Skip past it if it is
        ** a separator character. Or break out of the loop if it is not. */
        is = csr - (unsigned char*)text;
        READ_UTF8(csr, term, i_code);
        if( __fts5_unicode_is_alnum(p, i_code) ){
          goto non_ascii_tokenchar;
        }
      }else{
        if( a[*csr] ){
          is = csr - (unsigned char*)text;
          goto ascii_tokenchar;
        }
        csr++;
      }
    }

    /* Run through the tokenchars. Fold them into the output buffer along
    ** the way.  */
    while( csr<term ){

      /* Grow the output buffer so that there is sufficient space to fit the
      ** largest possible utf-8 character.  */
      if( out>end ){
        folds = dbsql_malloc64((int64_t)n_fold*2);
        if( folds==0 ){
          rc = DBSQL_NOMEM;
          goto tokenize_done;
        }
        out = &folds[out - p->folds];
        memcpy(folds, p->folds, n_fold);
        dbsql_free(p->folds);
        p->folds = folds;
        p->n_fold = n_fold = n_fold*2;
        end = &folds[n_fold-6];
      }

      if( *csr & 0x80 ){
        /* An non-ascii-range character. Fold it into the output buffer if
        ** it is a token character, or break out of the loop if it is not. */
        READ_UTF8(csr, term, i_code);
        if( __fts5_unicode_is_alnum(p,i_code)||__fts5_unicode_isdiacritic(i_code) ){
 non_ascii_tokenchar:
          i_code = __fts5_unicode_fold(i_code, p->remove_diacritic);
          if( i_code ) WRITE_UTF8(out, i_code);
        }else{
          break;
        }
      }else if( a[*csr]==0 ){
        /* An ascii-range separator character. End of token. */
        break; 
      }else{
 ascii_tokenchar:
        if( *csr>='A' && *csr<='Z' ){
          *out++ = *csr + 32;
        }else{
          *out++ = *csr;
        }
        csr++;
      }
      ie = csr - (unsigned char*)text;
    }

    /* Invoke the token callback */
    rc = token(ctx, 0, folds, out-folds, is, ie); 
  }
  
 tokenize_done:
  if( rc==DBSQL_DONE ) rc = DBSQL_OK;
  return rc;
}

/**************************************************************************
** Start of porter stemmer implementation.
*/

/* Any tokens larger than this (in bytes) are passed through without
** stemming. */
#define FTS5_PORTER_MAX_TOKEN 64

typedef struct porter_tokenizer_t porter_tokenizer_t;
struct porter_tokenizer_t {
  fts5_tokenizer_v2 tokenizer_v2; /* Parent tokenizer module */
  fts5_tokenizer_t *tokenizer;      /* Parent tokenizer instance */
  char bufs[FTS5_PORTER_MAX_TOKEN + 64];
};

/*
** Delete a "porter" tokenizer.
*/
static void __fts5_porter_delete(fts5_tokenizer_t *tok){
  if( tok ){
    porter_tokenizer_t *p = (porter_tokenizer_t*)tok;
    if( p->tokenizer ){
      p->tokenizer_v2.x_delete(p->tokenizer);
    }
    dbsql_free(p);
  }
}

/*
** Create a "porter" tokenizer.
*/
static int __fts5_porter_create(
  void *ctx, 
  const char **args, int n_arg,
  fts5_tokenizer_t **out
){
  fts5_api *api = (fts5_api*)ctx;
  int rc = DBSQL_OK;
  porter_tokenizer_t *ret;
  void *userdata = 0;
  const char *base = "unicode61";
  fts5_tokenizer_v2 *v2 = 0;

  while( n_arg>0 ){
    if( dbsql_stricmp(args[0],"porter")==0 ){
      n_arg--;
      args++;
    }else{
      base = args[0];
      break;
    }
  }

  ret = (porter_tokenizer_t*)dbsql_malloc64(sizeof(porter_tokenizer_t));
  if( ret ){
    memset(ret, 0, sizeof(porter_tokenizer_t));
    rc = api->find_tokenizer_v2(api, base, &userdata, &v2);
  }else{
    rc = DBSQL_NOMEM;
  }
  if( rc==DBSQL_OK ){
    int n_arg2 = (n_arg>0 ? n_arg-1 : 0);
    const char **az2 = (n_arg2 ? &args[1] : 0);
    memcpy(&ret->tokenizer_v2, v2, sizeof(fts5_tokenizer_v2));
    rc = ret->tokenizer_v2.create(userdata, az2, n_arg2, &ret->tokenizer);
  }

  if( rc!=DBSQL_OK ){
    __fts5_porter_delete((fts5_tokenizer_t*)ret);
    ret = 0;
  }
  *out = (fts5_tokenizer_t*)ret;
  return rc;
}

typedef struct porter_context_t porter_context_t;
struct porter_context_t {
  void *ctx;
  int (*x_token)(void*, int, const char*, int, int, int);
  char *bufs;
};

typedef struct porter_rule_t porter_rule_t;
struct porter_rule_t {
  const char *suffix;
  int n_suffix;
  int (*cond)(char *stem, int n_stem);
  const char *output;
  int n_output;
};

#if 0
static int __fts5_porter_apply(char *bufs, int *pn_buf, porter_rule_t *rules){
  int ret = -1;
  int n_buf = *pn_buf;
  porter_rule_t *p;

  for(p=rules; p->suffix; p++){
    assert( strlen(p->suffix)==p->n_suffix );
    assert( strlen(p->output)==p->n_output );
    if( n_buf<p->n_suffix ) continue;
    if( 0==memcmp(&bufs[n_buf - p->n_suffix], p->suffix, p->n_suffix) ) break;
  }

  if( p->suffix ){
    int n_stem = n_buf - p->n_suffix;
    if( p->cond==0 || p->cond(bufs, n_stem) ){
      memcpy(&bufs[n_stem], p->output, p->n_output);
      *pn_buf = n_stem + p->n_output;
      ret = p - rules;
    }
  }

  return ret;
}
#endif

static int __fts5_porter_is_vowel(char c, int y_is_vowel){
  return (
      c=='a' || c=='e' || c=='i' || c=='o' || c=='u' || (y_is_vowel && c=='y')
  );
}

static int __fts5_porter_gobble_vc(char *stem, int n_stem, int prev_cons){
  int i;
  int cons = prev_cons;

  /* Scan for a vowel */
  for(i=0; i<n_stem; i++){
    if( 0==(cons = !__fts5_porter_is_vowel(stem[i], cons)) ) break;
  }

  /* Scan for a consonent */
  for(i++; i<n_stem; i++){
    if( (cons = !__fts5_porter_is_vowel(stem[i], cons)) ) return i+1;
  }
  return 0;
}

/* porter rule condition: (m > 0) */
static int fts5Porter_MGt0(char *stem, int n_stem){
  return !!__fts5_porter_gobble_vc(stem, n_stem, 0);
}

/* porter rule condition: (m > 1) */
static int fts5Porter_MGt1(char *stem, int n_stem){
  int n;
  n = __fts5_porter_gobble_vc(stem, n_stem, 0);
  if( n && __fts5_porter_gobble_vc(&stem[n], n_stem-n, 1) ){
    return 1;
  }
  return 0;
}

/* porter rule condition: (m = 1) */
static int fts5Porter_MEq1(char *stem, int n_stem){
  int n;
  n = __fts5_porter_gobble_vc(stem, n_stem, 0);
  if( n && 0==__fts5_porter_gobble_vc(&stem[n], n_stem-n, 1) ){
    return 1;
  }
  return 0;
}

/* porter rule condition: (*o) */
static int fts5Porter_Ostar(char *stem, int n_stem){
  if( stem[n_stem-1]=='w' || stem[n_stem-1]=='x' || stem[n_stem-1]=='y' ){
    return 0;
  }else{
    int i;
    int mask = 0;
    int cons = 0;
    for(i=0; i<n_stem; i++){
      cons = !__fts5_porter_is_vowel(stem[i], cons);
      assert( cons==0 || cons==1 );
      mask = (mask << 1) + cons;
    }
    return ((mask & 0x0007)==0x0005);
  }
}

/* porter rule condition: (m > 1 and (*S or *T)) */
static int fts5Porter_MGt1_and_S_or_T(char *stem, int n_stem){
  assert( n_stem>0 );
  return (stem[n_stem-1]=='s' || stem[n_stem-1]=='t') 
      && fts5Porter_MGt1(stem, n_stem);
}

/* porter rule condition: (*v*) */
static int fts5Porter_Vowel(char *stem, int n_stem){
  int i;
  for(i=0; i<n_stem; i++){
    if( __fts5_porter_is_vowel(stem[i], i>0) ){
      return 1;
    }
  }
  return 0;
}


/**************************************************************************
***************************************************************************
** GENERATED CODE STARTS HERE (mkportersteps.tcl)
*/

static int __fts5_porter_step4(char *bufs, int *pn_buf){
  int ret = 0;
  int n_buf = *pn_buf;
  switch( bufs[n_buf-2] ){
    
    case 'a': 
      if( n_buf>2 && 0==memcmp("al", &bufs[n_buf-2], 2) ){
        if( fts5Porter_MGt1(bufs, n_buf-2) ){
          *pn_buf = n_buf - 2;
        }
      }
      break;
  
    case 'c': 
      if( n_buf>4 && 0==memcmp("ance", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt1(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }else if( n_buf>4 && 0==memcmp("ence", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt1(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }
      break;
  
    case 'e': 
      if( n_buf>2 && 0==memcmp("er", &bufs[n_buf-2], 2) ){
        if( fts5Porter_MGt1(bufs, n_buf-2) ){
          *pn_buf = n_buf - 2;
        }
      }
      break;
  
    case 'i': 
      if( n_buf>2 && 0==memcmp("ic", &bufs[n_buf-2], 2) ){
        if( fts5Porter_MGt1(bufs, n_buf-2) ){
          *pn_buf = n_buf - 2;
        }
      }
      break;
  
    case 'l': 
      if( n_buf>4 && 0==memcmp("able", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt1(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }else if( n_buf>4 && 0==memcmp("ible", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt1(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }
      break;
  
    case 'n': 
      if( n_buf>3 && 0==memcmp("ant", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }else if( n_buf>5 && 0==memcmp("ement", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt1(bufs, n_buf-5) ){
          *pn_buf = n_buf - 5;
        }
      }else if( n_buf>4 && 0==memcmp("ment", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt1(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }else if( n_buf>3 && 0==memcmp("ent", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 'o': 
      if( n_buf>3 && 0==memcmp("ion", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1_and_S_or_T(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }else if( n_buf>2 && 0==memcmp("ou", &bufs[n_buf-2], 2) ){
        if( fts5Porter_MGt1(bufs, n_buf-2) ){
          *pn_buf = n_buf - 2;
        }
      }
      break;
  
    case 's': 
      if( n_buf>3 && 0==memcmp("ism", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 't': 
      if( n_buf>3 && 0==memcmp("ate", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }else if( n_buf>3 && 0==memcmp("iti", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 'u': 
      if( n_buf>3 && 0==memcmp("ous", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 'v': 
      if( n_buf>3 && 0==memcmp("ive", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 'z': 
      if( n_buf>3 && 0==memcmp("ize", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt1(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
  }
  return ret;
}
  

static int __fts5_porter_step1_b2(char *bufs, int *pn_buf){
  int ret = 0;
  int n_buf = *pn_buf;
  switch( bufs[n_buf-2] ){
    
    case 'a': 
      if( n_buf>2 && 0==memcmp("at", &bufs[n_buf-2], 2) ){
        memcpy(&bufs[n_buf-2], "ate", 3);
        *pn_buf = n_buf - 2 + 3;
        ret = 1;
      }
      break;
  
    case 'b': 
      if( n_buf>2 && 0==memcmp("bl", &bufs[n_buf-2], 2) ){
        memcpy(&bufs[n_buf-2], "ble", 3);
        *pn_buf = n_buf - 2 + 3;
        ret = 1;
      }
      break;
  
    case 'i': 
      if( n_buf>2 && 0==memcmp("iz", &bufs[n_buf-2], 2) ){
        memcpy(&bufs[n_buf-2], "ize", 3);
        *pn_buf = n_buf - 2 + 3;
        ret = 1;
      }
      break;
  
  }
  return ret;
}
  

static int __fts5_porter_step2(char *bufs, int *pn_buf){
  int ret = 0;
  int n_buf = *pn_buf;
  switch( bufs[n_buf-2] ){
    
    case 'a': 
      if( n_buf>7 && 0==memcmp("ational", &bufs[n_buf-7], 7) ){
        if( fts5Porter_MGt0(bufs, n_buf-7) ){
          memcpy(&bufs[n_buf-7], "ate", 3);
          *pn_buf = n_buf - 7 + 3;
        }
      }else if( n_buf>6 && 0==memcmp("tional", &bufs[n_buf-6], 6) ){
        if( fts5Porter_MGt0(bufs, n_buf-6) ){
          memcpy(&bufs[n_buf-6], "tion", 4);
          *pn_buf = n_buf - 6 + 4;
        }
      }
      break;
  
    case 'c': 
      if( n_buf>4 && 0==memcmp("enci", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "ence", 4);
          *pn_buf = n_buf - 4 + 4;
        }
      }else if( n_buf>4 && 0==memcmp("anci", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "ance", 4);
          *pn_buf = n_buf - 4 + 4;
        }
      }
      break;
  
    case 'e': 
      if( n_buf>4 && 0==memcmp("izer", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "ize", 3);
          *pn_buf = n_buf - 4 + 3;
        }
      }
      break;
  
    case 'g': 
      if( n_buf>4 && 0==memcmp("logi", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "log", 3);
          *pn_buf = n_buf - 4 + 3;
        }
      }
      break;
  
    case 'l': 
      if( n_buf>3 && 0==memcmp("bli", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt0(bufs, n_buf-3) ){
          memcpy(&bufs[n_buf-3], "ble", 3);
          *pn_buf = n_buf - 3 + 3;
        }
      }else if( n_buf>4 && 0==memcmp("alli", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "al", 2);
          *pn_buf = n_buf - 4 + 2;
        }
      }else if( n_buf>5 && 0==memcmp("entli", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ent", 3);
          *pn_buf = n_buf - 5 + 3;
        }
      }else if( n_buf>3 && 0==memcmp("eli", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt0(bufs, n_buf-3) ){
          memcpy(&bufs[n_buf-3], "e", 1);
          *pn_buf = n_buf - 3 + 1;
        }
      }else if( n_buf>5 && 0==memcmp("ousli", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ous", 3);
          *pn_buf = n_buf - 5 + 3;
        }
      }
      break;
  
    case 'o': 
      if( n_buf>7 && 0==memcmp("ization", &bufs[n_buf-7], 7) ){
        if( fts5Porter_MGt0(bufs, n_buf-7) ){
          memcpy(&bufs[n_buf-7], "ize", 3);
          *pn_buf = n_buf - 7 + 3;
        }
      }else if( n_buf>5 && 0==memcmp("ation", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ate", 3);
          *pn_buf = n_buf - 5 + 3;
        }
      }else if( n_buf>4 && 0==memcmp("ator", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "ate", 3);
          *pn_buf = n_buf - 4 + 3;
        }
      }
      break;
  
    case 's': 
      if( n_buf>5 && 0==memcmp("alism", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "al", 2);
          *pn_buf = n_buf - 5 + 2;
        }
      }else if( n_buf>7 && 0==memcmp("iveness", &bufs[n_buf-7], 7) ){
        if( fts5Porter_MGt0(bufs, n_buf-7) ){
          memcpy(&bufs[n_buf-7], "ive", 3);
          *pn_buf = n_buf - 7 + 3;
        }
      }else if( n_buf>7 && 0==memcmp("fulness", &bufs[n_buf-7], 7) ){
        if( fts5Porter_MGt0(bufs, n_buf-7) ){
          memcpy(&bufs[n_buf-7], "ful", 3);
          *pn_buf = n_buf - 7 + 3;
        }
      }else if( n_buf>7 && 0==memcmp("ousness", &bufs[n_buf-7], 7) ){
        if( fts5Porter_MGt0(bufs, n_buf-7) ){
          memcpy(&bufs[n_buf-7], "ous", 3);
          *pn_buf = n_buf - 7 + 3;
        }
      }
      break;
  
    case 't': 
      if( n_buf>5 && 0==memcmp("aliti", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "al", 2);
          *pn_buf = n_buf - 5 + 2;
        }
      }else if( n_buf>5 && 0==memcmp("iviti", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ive", 3);
          *pn_buf = n_buf - 5 + 3;
        }
      }else if( n_buf>6 && 0==memcmp("biliti", &bufs[n_buf-6], 6) ){
        if( fts5Porter_MGt0(bufs, n_buf-6) ){
          memcpy(&bufs[n_buf-6], "ble", 3);
          *pn_buf = n_buf - 6 + 3;
        }
      }
      break;
  
  }
  return ret;
}
  

static int __fts5_porter_step3(char *bufs, int *pn_buf){
  int ret = 0;
  int n_buf = *pn_buf;
  switch( bufs[n_buf-2] ){
    
    case 'a': 
      if( n_buf>4 && 0==memcmp("ical", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          memcpy(&bufs[n_buf-4], "ic", 2);
          *pn_buf = n_buf - 4 + 2;
        }
      }
      break;
  
    case 's': 
      if( n_buf>4 && 0==memcmp("ness", &bufs[n_buf-4], 4) ){
        if( fts5Porter_MGt0(bufs, n_buf-4) ){
          *pn_buf = n_buf - 4;
        }
      }
      break;
  
    case 't': 
      if( n_buf>5 && 0==memcmp("icate", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ic", 2);
          *pn_buf = n_buf - 5 + 2;
        }
      }else if( n_buf>5 && 0==memcmp("iciti", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "ic", 2);
          *pn_buf = n_buf - 5 + 2;
        }
      }
      break;
  
    case 'u': 
      if( n_buf>3 && 0==memcmp("ful", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt0(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
        }
      }
      break;
  
    case 'v': 
      if( n_buf>5 && 0==memcmp("ative", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          *pn_buf = n_buf - 5;
        }
      }
      break;
  
    case 'z': 
      if( n_buf>5 && 0==memcmp("alize", &bufs[n_buf-5], 5) ){
        if( fts5Porter_MGt0(bufs, n_buf-5) ){
          memcpy(&bufs[n_buf-5], "al", 2);
          *pn_buf = n_buf - 5 + 2;
        }
      }
      break;
  
  }
  return ret;
}
  

static int __fts5_porter_step1_b(char *bufs, int *pn_buf){
  int ret = 0;
  int n_buf = *pn_buf;
  switch( bufs[n_buf-2] ){
    
    case 'e': 
      if( n_buf>3 && 0==memcmp("eed", &bufs[n_buf-3], 3) ){
        if( fts5Porter_MGt0(bufs, n_buf-3) ){
          memcpy(&bufs[n_buf-3], "ee", 2);
          *pn_buf = n_buf - 3 + 2;
        }
      }else if( n_buf>2 && 0==memcmp("ed", &bufs[n_buf-2], 2) ){
        if( fts5Porter_Vowel(bufs, n_buf-2) ){
          *pn_buf = n_buf - 2;
          ret = 1;
        }
      }
      break;
  
    case 'n': 
      if( n_buf>3 && 0==memcmp("ing", &bufs[n_buf-3], 3) ){
        if( fts5Porter_Vowel(bufs, n_buf-3) ){
          *pn_buf = n_buf - 3;
          ret = 1;
        }
      }
      break;
  
  }
  return ret;
}
  
/* 
** GENERATED CODE ENDS HERE (mkportersteps.tcl)
***************************************************************************
**************************************************************************/

static void __fts5_porter_step1_a(char *bufs, int *pn_buf){
  int n_buf = *pn_buf;
  if( bufs[n_buf-1]=='s' ){
    if( bufs[n_buf-2]=='e' ){
      if( (n_buf>4 && bufs[n_buf-4]=='s' && bufs[n_buf-3]=='s') 
       || (n_buf>3 && bufs[n_buf-3]=='i' )
      ){
        *pn_buf = n_buf-2;
      }else{
        *pn_buf = n_buf-1;
      }
    }
    else if( bufs[n_buf-2]!='s' ){
      *pn_buf = n_buf-1;
    }
  }
}

static int __fts5_porter_cb(
  void *ctx, 
  int tflags,
  const char *token, 
  int n_token, 
  int i_start, 
  int i_end
){
  porter_context_t *p = (porter_context_t*)ctx;

  char *bufs;
  int n_buf;

  if( n_token>FTS5_PORTER_MAX_TOKEN || n_token<3 ) goto pass_through;
  bufs = p->bufs;
  n_buf = n_token;
  memcpy(bufs, token, n_buf);

  /* Step 1. */
  __fts5_porter_step1_a(bufs, &n_buf);
  if( __fts5_porter_step1_b(bufs, &n_buf) ){
    if( __fts5_porter_step1_b2(bufs, &n_buf)==0 ){
      char c = bufs[n_buf-1];
      if( __fts5_porter_is_vowel(c, 0)==0 
       && c!='l' && c!='s' && c!='z' && c==bufs[n_buf-2] 
      ){
        n_buf--;
      }else if( fts5Porter_MEq1(bufs, n_buf) && fts5Porter_Ostar(bufs, n_buf) ){
        bufs[n_buf++] = 'e';
      }
    }
  }

  /* Step 1C. */
  if( bufs[n_buf-1]=='y' && fts5Porter_Vowel(bufs, n_buf-1) ){
    bufs[n_buf-1] = 'i';
  }

  /* Steps 2 through 4. */
  __fts5_porter_step2(bufs, &n_buf);
  __fts5_porter_step3(bufs, &n_buf);
  __fts5_porter_step4(bufs, &n_buf);

  /* Step 5a. */
  assert( n_buf>0 );
  if( bufs[n_buf-1]=='e' ){
    if( fts5Porter_MGt1(bufs, n_buf-1) 
     || (fts5Porter_MEq1(bufs, n_buf-1) && !fts5Porter_Ostar(bufs, n_buf-1))
    ){
      n_buf--;
    }
  }

  /* Step 5b. */
  if( n_buf>1 && bufs[n_buf-1]=='l' 
   && bufs[n_buf-2]=='l' && fts5Porter_MGt1(bufs, n_buf-1) 
  ){
    n_buf--;
  }

  return p->x_token(p->ctx, tflags, bufs, n_buf, i_start, i_end);

 pass_through:
  return p->x_token(p->ctx, tflags, token, n_token, i_start, i_end);
}

/*
** Tokenize using the porter tokenizer.
*/
static int __fts5_porter_tokenize(
  fts5_tokenizer_t *tokenizer,
  void *ctx,
  int flags,
  const char *text, int n_text,
  const char *loc, int n_loc,
  int (*token)(void*, int, const char*, int n_token, int i_start, int i_end)
){
  porter_tokenizer_t *p = (porter_tokenizer_t*)tokenizer;
  porter_context_t s_ctx;
  s_ctx.x_token = token;
  s_ctx.ctx = ctx;
  s_ctx.bufs = p->bufs;
  return p->tokenizer_v2.tokenize(
      p->tokenizer, (void*)&s_ctx, flags, text, n_text, loc, n_loc, __fts5_porter_cb
  );
}

/**************************************************************************
** Start of trigram implementation.
*/
typedef struct trigram_tokenizer_t trigram_tokenizer_t;
struct trigram_tokenizer_t {
  int fold;                      /* True to fold to lower-case */
  int i_fold_param;                 /* Parameter to pass to __fts5_unicode_fold() */
};

/*
** Free a trigram tokenizer.
*/
static void __fts5_tri_delete(fts5_tokenizer_t *p){
  dbsql_free(p);
}

/*
** Allocate a trigram tokenizer.
*/
static int __fts5_tri_create(
  void *unused,
  const char **args,
  int n_arg,
  fts5_tokenizer_t **out
){
  int rc = DBSQL_OK;
  trigram_tokenizer_t *p_new = 0;
  UNUSED_PARAM(unused);
  if( n_arg%2 ){
    rc = DBSQL_ERROR;
  }else{
    int i;
    p_new = (trigram_tokenizer_t*)dbsql_malloc64(sizeof(*p_new));
    if( p_new==0 ){
      rc = DBSQL_NOMEM;
    }else{
      p_new->fold = 1;
      p_new->i_fold_param = 0;
  
      for(i=0; rc==DBSQL_OK && i<n_arg; i+=2){
        const char *arg = args[i+1];
        if( 0==dbsql_stricmp(args[i], "case_sensitive") ){
          if( (arg[0]!='0' && arg[0]!='1') || arg[1] ){
            rc = DBSQL_ERROR;
          }else{
            p_new->fold = (arg[0]=='0');
          }
        }else if( 0==dbsql_stricmp(args[i], "remove_diacritics") ){
          if( (arg[0]!='0' && arg[0]!='1' && arg[0]!='2') || arg[1] ){
            rc = DBSQL_ERROR;
          }else{
            p_new->i_fold_param = (arg[0]!='0') ? 2 : 0;
          }
        }else{
          rc = DBSQL_ERROR;
        }
      }
  
      if( p_new->i_fold_param!=0 && p_new->fold==0 ){
        rc = DBSQL_ERROR;
      }
  
      if( rc!=DBSQL_OK ){
        __fts5_tri_delete((fts5_tokenizer_t*)p_new);
        p_new = 0;
      }
    }
  }
  *out = (fts5_tokenizer_t*)p_new;
  return rc;
}

/*
** Trigram tokenizer tokenize routine.
*/
static int __fts5_tri_tokenize(
  fts5_tokenizer_t *tok,
  void *ctx,
  int unused_flags,
  const char *text, int n_text,
  int (*token)(void*, int, const char*, int, int, int)
){
  trigram_tokenizer_t *p = (trigram_tokenizer_t*)tok;
  int rc = DBSQL_OK;
  char bufs[32];
  char *out = bufs;
  int ii;
  const unsigned char *in = (const unsigned char*)text;
  const unsigned char *eof = (in ? &in[n_text] : 0);
  u_int32_t i_code = 0;
  int starts[3];                  /* Input offset of each character in bufs[] */

  UNUSED_PARAM(unused_flags);

  /* Populate bufs[] with the characters for the first trigram. */
  for(ii=0; ii<3; ii++){
    do {
      starts[ii] = in - (const unsigned char*)text;
      if( in>=eof ) return DBSQL_OK;
      READ_UTF8(in, eof, i_code);
      if( p->fold ) i_code = __fts5_unicode_fold(i_code, p->i_fold_param);
    }while( i_code==0 );
    WRITE_UTF8(out, i_code);
  }

  /* At the start of each iteration of this loop:
  **
  **  bufs:      Contains 3 characters. The 3 characters of the next trigram.
  **  out:      Points to the byte following the last character in bufs.
  **  starts[3]: Contains the byte offset in the input text corresponding
  **             to the start of each of the three characters in the buffer.
  */
  assert( in<=eof );
  while( 1 ){
    int i_next;                    /* Start of character following current tri */
    const char *z1;

    /* Read characters from the input up until the first non-diacritic */
    do {
      i_next = in - (const unsigned char*)text;
      if( in>=eof ){
        i_code = 0;
        break;
      }
      READ_UTF8(in, eof, i_code);
      if( p->fold ) i_code = __fts5_unicode_fold(i_code, p->i_fold_param);
    }while( i_code==0 );

    /* Pass the current trigram back to fts5 */
    rc = token(ctx, 0, bufs, out-bufs, starts[0], i_next);
    if( i_code==0 || rc!=DBSQL_OK ) break;

    /* Remove the first character from buffer bufs[]. Append the character
    ** with codepoint i_code.  */
    z1 = bufs;
    FTS5_SKIP_UTF8(z1);
    memmove(bufs, z1, out - z1);
    out -= (z1 - bufs);
    WRITE_UTF8(out, i_code);

    /* Update the starts[] array */
    starts[0] = starts[1];
    starts[1] = starts[2];
    starts[2] = i_next;
  }

  return rc;
}

/*
** Argument create is a pointer to a constructor function for a tokenizer.
** tok is a tokenizer previously created using the same method. This function
** returns one of FTS5_PATTERN_NONE, FTS5_PATTERN_LIKE or FTS5_PATTERN_GLOB
** indicating the style of pattern matching that the tokenizer can support.
** In practice, this is:
**
**     "trigram" tokenizer, case_sensitive=1 - FTS5_PATTERN_GLOB
**     "trigram" tokenizer, case_sensitive=0 (the default) - FTS5_PATTERN_LIKE
**     all other tokenizers - FTS5_PATTERN_NONE
*/
int __fts5_tokenizer_pattern(
    int (*create)(void*, const char**, int, fts5_tokenizer_t**),
    fts5_tokenizer_t *tok
){
  if( create==__fts5_tri_create ){
    trigram_tokenizer_t *p = (trigram_tokenizer_t*)tok;
    if( p->i_fold_param==0 ){
      return p->fold ? FTS5_PATTERN_LIKE : FTS5_PATTERN_GLOB;
    }
  }
  return FTS5_PATTERN_NONE;
}

/*
** Return true if the tokenizer described by p->args[] is the trigram
** tokenizer. This tokenizer needs to be loaded before best_index is
** called for the first time in order to correctly handle LIKE/GLOB.
*/
int __fts5_tokenizer_preload(fts5_tokenizer_config_t *p){
  return (p->n_arg>=1 && 0==dbsql_stricmp(p->args[0], "trigram"));
}


/*
** Register all built-in tokenizers with FTS5.
*/
int __fts5_tokenizer_init(fts5_api *api){
  struct builtin_tokenizer_t {
    const char *name;
    fts5_tokenizer x;
  } builtins[] = {
    { "unicode61", {__fts5_unicode_create, __fts5_unicode_delete, __fts5_unicode_tokenize}},
    { "ascii",     {__fts5_ascii_create, __fts5_ascii_delete, __fts5_ascii_tokenize }},
    { "trigram",   {__fts5_tri_create, __fts5_tri_delete, __fts5_tri_tokenize}},
  };
  
  int rc = DBSQL_OK;             /* Return code */
  int i;                          /* To iterate through builtin functions */

  for(i=0; rc==DBSQL_OK && i<ARRAY_SIZE(builtins); i++){
    rc = api->create_tokenizer(api,
        builtins[i].name,
        (void*)api,
        &builtins[i].x,
        0
    );
  }
  if( rc==DBSQL_OK ){
    fts5_tokenizer_v2 s_porter = {
      2,
      __fts5_porter_create,
      __fts5_porter_delete,
      __fts5_porter_tokenize
    };
    rc = api->create_tokenizer_v2(api,
        "porter",
        (void*)api,
        &s_porter,
        0
    );
  }
  return rc;
}
