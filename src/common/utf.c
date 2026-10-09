/*
** 2004 April 13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains routines used to translate between UTF-8, 
** UTF-16, UTF-16BE, and UTF-16LE.
**
** Notes on UTF-8:
**
**   Byte-0    Byte-1    Byte-2    Byte-3    Value
**  0xxxxxxx                                 00000000 00000000 0xxxxxxx
**  110yyyyy  10xxxxxx                       00000000 00000yyy yyxxxxxx
**  1110zzzz  10yyyyyy  10xxxxxx             00000000 zzzzyyyy yyxxxxxx
**  11110uuu  10uuzzzz  10yyyyyy  10xxxxxx   000uuuuu zzzzyyyy yyxxxxxx
**
**
** Notes on UTF-16:  (with wwww+1==uuuuu)
**
**      Word-0               Word-1          Value
**  110110ww wwzzzzyy   110111yy yyxxxxxx    000uuuuu zzzzyyyy yyxxxxxx
**  zzzzyyyy yyxxxxxx                        00000000 zzzzyyyy yyxxxxxx
**
**
** BOM or Byte Order Mark:
**     0xff 0xfe   little-endian utf-16 follows
**     0xfe 0xff   big-endian utf-16 follows
**
*/
#include "dbsql_int.h"
#include <assert.h>
#include "vdbe_int.h"

#if !defined(DBSQL_AMALGAMATION) && DBSQL_BYTEORDER==0
/*
** The following constant value is used by the DBSQL_BIGENDIAN and
** DBSQL_LITTLEENDIAN macros.
*/
const int __dbsqlone = 1;
#endif /* DBSQL_AMALGAMATION && DBSQL_BYTEORDER==0 */

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


#define WRITE_UTF8(out, c) {                          \
  if( c<0x00080 ){                                     \
    *out++ = (u_int8_t)(c&0xFF);                            \
  }                                                    \
  else if( c<0x00800 ){                                \
    *out++ = 0xC0 + (u_int8_t)((c>>6)&0x1F);                \
    *out++ = 0x80 + (u_int8_t)(c & 0x3F);                   \
  }                                                    \
  else if( c<0x10000 ){                                \
    *out++ = 0xE0 + (u_int8_t)((c>>12)&0x0F);               \
    *out++ = 0x80 + (u_int8_t)((c>>6) & 0x3F);              \
    *out++ = 0x80 + (u_int8_t)(c & 0x3F);                   \
  }else{                                               \
    *out++ = 0xF0 + (u_int8_t)((c>>18) & 0x07);             \
    *out++ = 0x80 + (u_int8_t)((c>>12) & 0x3F);             \
    *out++ = 0x80 + (u_int8_t)((c>>6) & 0x3F);              \
    *out++ = 0x80 + (u_int8_t)(c & 0x3F);                   \
  }                                                    \
}

#define WRITE_UTF16LE(out, c) {                                    \
  if( c<=0xFFFF ){                                                  \
    *out++ = (u_int8_t)(c&0x00FF);                                       \
    *out++ = (u_int8_t)((c>>8)&0x00FF);                                  \
  }else{                                                            \
    *out++ = (u_int8_t)(((c>>10)&0x003F) + (((c-0x10000)>>10)&0x00C0));  \
    *out++ = (u_int8_t)(0x00D8 + (((c-0x10000)>>18)&0x03));              \
    *out++ = (u_int8_t)(c&0x00FF);                                       \
    *out++ = (u_int8_t)(0x00DC + ((c>>8)&0x03));                         \
  }                                                                 \
}

#define WRITE_UTF16BE(out, c) {                                    \
  if( c<=0xFFFF ){                                                  \
    *out++ = (u_int8_t)((c>>8)&0x00FF);                                  \
    *out++ = (u_int8_t)(c&0x00FF);                                       \
  }else{                                                            \
    *out++ = (u_int8_t)(0x00D8 + (((c-0x10000)>>18)&0x03));              \
    *out++ = (u_int8_t)(((c>>10)&0x003F) + (((c-0x10000)>>10)&0x00C0));  \
    *out++ = (u_int8_t)(0x00DC + ((c>>8)&0x03));                         \
    *out++ = (u_int8_t)(c&0x00FF);                                       \
  }                                                                 \
}

/*
** Write a single UTF8 character whose value is v into the
** buffer starting at out.  out must be sized to hold at
** least four bytes.  Return the number of bytes needed
** to encode the new character.
*/
int __append_one_utf8_character(char *out, u_int32_t v){
  if( v<0x00080 ){
    out[0] = (u_int8_t)(v & 0xff);
    return 1;
  }
  if( v<0x00800 ){
    out[0] = 0xc0 + (u_int8_t)((v>>6) & 0x1f);
    out[1] = 0x80 + (u_int8_t)(v & 0x3f);
    return 2;
  }
  if( v<0x10000 ){
    out[0] = 0xe0 + (u_int8_t)((v>>12) & 0x0f);
    out[1] = 0x80 + (u_int8_t)((v>>6) & 0x3f);
    out[2] = 0x80 + (u_int8_t)(v & 0x3f);
    return 3;
  }
  out[0] = 0xf0 + (u_int8_t)((v>>18) & 0x07);
  out[1] = 0x80 + (u_int8_t)((v>>12) & 0x3f);
  out[2] = 0x80 + (u_int8_t)((v>>6) & 0x3f);
  out[3] = 0x80 + (u_int8_t)(v & 0x3f);
  return 4;
}

/*
** Translate a single UTF-8 character.  Return the unicode value.
**
** During translation, assume that the byte that term points
** is a 0x00.
**
** Write a pointer to the next unread byte back into *next.
**
** Notes On Invalid UTF-8:
**
**  *  This routine never allows a 7-bit character (0x00 through 0x7f) to
**     be encoded as a multi-byte character.  Any multi-byte character that
**     attempts to encode a value between 0x00 and 0x7f is rendered as 0xfffd.
**
**  *  This routine never allows a UTF16 surrogate value to be encoded.
**     If a multi-byte character attempts to encode a value between
**     0xd800 and 0xe000 then it is rendered as 0xfffd.
**
**  *  Bytes in the range of 0x80 through 0xbf which occur as the first
**     byte of a character are interpreted as single-byte characters
**     and rendered as themselves even though they are technically
**     invalid characters.
**
**  *  This routine accepts over-length UTF8 encodings
**     for unicode values 0x80 and greater.  It does not change over-length
**     encodings to 0xfffd as some systems recommend.
*/
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
u_int32_t __utf8_read(
  const unsigned char **pz    /* Pointer to string from which to read char */
){
  unsigned int c;

  /* Same as READ_UTF8() above but without the term parameter.
  ** For this routine, we assume the UTF8 string is always zero-terminated.
  */
  c = *((*pz)++);
  if( c>=0xc0 ){
    c = __utf8_trans1[c-0xc0];
    while( (*(*pz) & 0xc0)==0x80 ){
      c = (c<<6) + (0x3f & *((*pz)++));
    }
    if( c<0x80
        || (c&0xFFFFF800)==0xD800
        || (c&0xFFFFFFFE)==0xFFFE ){  c = 0xFFFD; }
  }
  return c;
}

/*
** Read a single UTF8 character out of buffer z[], but reading no
** more than n characters from the buffer.  z[] is not zero-terminated.
**
** Return the number of bytes used to construct the character.
**
** Invalid UTF8 might generate a strange result.  No effort is made
** to detect invalid UTF8.
**
** At most 4 bytes will be read out of z[].  The return value will always
** be between 1 and 4.
*/
int __utf8_read_limited(
  const u_int8_t *z,
  int n,
  u_int32_t *pi_out
){
  u_int32_t c;
  int i = 1;
  assert( n>0 );
  c = z[0];
  if( c>=0xc0 ){
    c = __utf8_trans1[c-0xc0];
    if( n>4 ) n = 4;
    while( i<n && (z[i] & 0xc0)==0x80 ){
      c = (c<<6) + (0x3f & z[i]);
      i++;
    }
  }
  *pi_out = c;
  return i;
}


/*
** If the TRANSLATE_TRACE macro is defined, the value of each mem_t is
** printed on stderr on the way into and out of __vdbe_mem_translate().
*/ 
/* #define TRANSLATE_TRACE 1 */

#ifndef DBSQL_OMIT_UTF16
/*
** This routine transforms the internal text encoding used by mem to
** desired_enc. It is an error if the string is already of the desired
** encoding, or if *mem does not contain a string value.
*/
DBSQL_NOINLINE int __vdbe_mem_translate(mem_t *mem, u_int8_t desired_enc){
  int64_t len;          /* Maximum length of output string in bytes */
  unsigned char *out;        /* Output buffer */
  unsigned char *in;         /* Input iterator */
  unsigned char *term;       /* End of input */
  unsigned char *z;           /* Output iterator */
  unsigned int c;

  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( mem->flags&MEM_Str );
  assert( mem->enc!=desired_enc );
  assert( mem->enc!=0 );
  assert( mem->n>=0 );

#if defined(TRANSLATE_TRACE) && defined(DBSQL_DEBUG)
  {
    str_accum_t acc;
    char buf[1000];
    __str_accum_init(&acc, 0, buf, sizeof(buf), 0);  
    __vdbe_mem_pretty_print(mem, &acc);
    fprintf(stderr, "INPUT:  %s\n", __str_accum_finish(&acc));
  }
#endif

  /* If the translation is between UTF-16 little and big endian, then 
  ** all that is required is to swap the byte order. This case is handled
  ** differently from the others.
  */
  if( mem->enc!=DBSQL_UTF8 && desired_enc!=DBSQL_UTF8 ){
    u_int8_t temp;
    int rc;
    rc = __vdbe_mem_make_writeable(mem);
    if( rc!=DBSQL_OK ){
      assert( rc==DBSQL_NOMEM );
      return DBSQL_NOMEM_BKPT;
    }
    in = (u_int8_t*)mem->z;
    term = &in[mem->n&~1];
    while( in<term ){
      temp = *in;
      *in = *(in+1);
      in++;
      *in++ = temp;
    }
    mem->enc = desired_enc;
    goto translate_out;
  }

  /* Set len to the maximum number of bytes required in the output buffer. */
  if( desired_enc==DBSQL_UTF8 ){
    /* When converting from UTF-16, the maximum growth results from
    ** translating a 2-byte character to a 4-byte UTF-8 character.
    ** A single byte is required for the output string
    ** nul-terminator.
    */
    mem->n &= ~1;
    len = 2 * (int64_t)mem->n + 1;
  }else{
    /* When converting from UTF-8 to UTF-16 the maximum growth is caused
    ** when a 1-byte UTF-8 character is translated into a 2-byte UTF-16
    ** character. Two bytes are required in the output buffer for the
    ** nul-terminator.
    */
    len = 2 * (int64_t)mem->n + 2;
  }

  /* Set in to point at the start of the input buffer and term to point 1
  ** byte past the end.
  **
  ** Variable out is set to point at the output buffer, space obtained
  ** from dbsql_malloc().
  */
  in = (u_int8_t*)mem->z;
  term = &in[mem->n];
  out = __db_malloc_raw(mem->db, len);
  if( !out ){
    return DBSQL_NOMEM_BKPT;
  }
  z = out;

  if( mem->enc==DBSQL_UTF8 ){
    if( desired_enc==DBSQL_UTF16LE ){
      /* UTF-8 -> UTF-16 Little-endian */
      while( in<term ){
        READ_UTF8(in, term, c);
        WRITE_UTF16LE(z, c);
      }
    }else{
      assert( desired_enc==DBSQL_UTF16BE );
      /* UTF-8 -> UTF-16 Big-endian */
      while( in<term ){
        READ_UTF8(in, term, c);
        WRITE_UTF16BE(z, c);
      }
    }
    mem->n = (int)(z - out);
    *z++ = 0;
  }else{
    assert( desired_enc==DBSQL_UTF8 );
    if( mem->enc==DBSQL_UTF16LE ){
      /* UTF-16 Little-endian -> UTF-8 */
      while( in<term ){
        c = *(in++);
        c += (*(in++))<<8;
        if( c>=0xd800 && c<0xe000 ){
#ifdef DBSQL_REPLACE_INVALID_UTF
          if( c>=0xdc00 || in>=term ){
            c = 0xfffd;
          }else{
            int c2 = *(in++);
            c2 += (*(in++))<<8;
            if( c2<0xdc00 || c2>=0xe000 ){
              in -= 2;
              c = 0xfffd;
            }else{
              c = ((c&0x3ff)<<10) + (c2&0x3ff) + 0x10000;
            }
          }
#else
          if( in<term ){
            int c2 = (*in++);
            c2 += ((*in++)<<8);
            c = (c2&0x03FF) + ((c&0x003F)<<10) + (((c&0x03C0)+0x0040)<<10);
          }
#endif
        }
        WRITE_UTF8(z, c);
      }
    }else{
      /* UTF-16 Big-endian -> UTF-8 */
      while( in<term ){
        c = (*(in++))<<8;
        c += *(in++);
        if( c>=0xd800 && c<0xe000 ){
#ifdef DBSQL_REPLACE_INVALID_UTF
          if( c>=0xdc00 || in>=term ){
            c = 0xfffd;
          }else{
            int c2 = (*(in++))<<8;
            c2 += *(in++);
            if( c2<0xdc00 || c2>=0xe000 ){
              in -= 2;
              c = 0xfffd;
            }else{
              c = ((c&0x3ff)<<10) + (c2&0x3ff) + 0x10000;
            }
          }
#else
          if( in<term ){
            int c2 = ((*in++)<<8);
            c2 += (*in++);
            c = (c2&0x03FF) + ((c&0x003F)<<10) + (((c&0x03C0)+0x0040)<<10);
          }
#endif
        }
        WRITE_UTF8(z, c);
      }
    }
    mem->n = (int)(z - out);
  }
  *z = 0;
  assert( (mem->n+(desired_enc==DBSQL_UTF8?1:2))<=len );

  c = MEM_Str|MEM_Term|(mem->flags&(MEM_AffMask|MEM_Subtype));
  __vdbe_mem_release(mem);
  mem->flags = c;
  mem->enc = desired_enc;
  mem->z = (char*)out;
  mem->z_malloc = mem->z;
  mem->sz_malloc = __db_malloc_size(mem->db, mem->z);

translate_out:
#if defined(TRANSLATE_TRACE) && defined(DBSQL_DEBUG)
  {
    str_accum_t acc;
    char buf[1000];
    __str_accum_init(&acc, 0, buf, sizeof(buf), 0);  
    __vdbe_mem_pretty_print(mem, &acc);
    fprintf(stderr, "OUTPUT: %s\n", __str_accum_finish(&acc));
  }
#endif
  return DBSQL_OK;
}
#endif /* DBSQL_OMIT_UTF16 */

#ifndef DBSQL_OMIT_UTF16
/*
** This routine checks for a byte-order mark at the beginning of the 
** UTF-16 string stored in *mem. If one is present, it is removed and
** the encoding of the mem_t adjusted. This routine does not do any
** byte-swapping, it just sets mem_t.enc appropriately.
**
** The allocation (static, dynamic etc.) and encoding of the mem_t may be
** changed by this function.
*/
int __vdbe_mem_handle_bom(mem_t *mem){
  int rc = DBSQL_OK;
  u_int8_t bom = 0;

  assert( mem->n>=0 );
  if( mem->n>1 ){
    u_int8_t b1 = *(u_int8_t *)mem->z;
    u_int8_t b2 = *(((u_int8_t *)mem->z) + 1);
    if( b1==0xFE && b2==0xFF ){
      bom = DBSQL_UTF16BE;
    }
    if( b1==0xFF && b2==0xFE ){
      bom = DBSQL_UTF16LE;
    }
  }
  
  if( bom ){
    rc = __vdbe_mem_make_writeable(mem);
    if( rc==DBSQL_OK ){
      mem->n -= 2;
      memmove(mem->z, &mem->z[2], mem->n);
      mem->z[mem->n] = '\0';
      mem->z[mem->n+1] = '\0';
      mem->flags |= MEM_Term;
      mem->enc = bom;
    }
  }
  return rc;
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** z is a UTF-8 encoded unicode string. If n_byte is less than zero,
** return the number of unicode characters in z up to (but not including)
** the first 0x00 byte. If n_byte is not less than zero, return the
** number of unicode characters in the first n_byte of z (or up to 
** the first 0x00, whichever comes first).
*/
int __utf8_char_len(const char *in, int n_byte){
  int r = 0;
  const u_int8_t *z = (const u_int8_t*)in;
  const u_int8_t *term;
  if( n_byte>=0 ){
    term = &z[n_byte];
  }else{
    term = (const u_int8_t*)(-1);
  }
  assert( z<=term );
  while( *z!=0 && z<term ){
    DBSQL_SKIP_UTF8(z);
    r++;
  }
  return r;
}

/* This test function is not currently used by the automated test-suite. 
** Hence it is only available in debug builds.
*/
#if defined(DBSQL_TEST) && defined(DBSQL_DEBUG)
/*
** Translate UTF-8 to UTF-8.
**
** This has the effect of making sure that the string is well-formed
** UTF-8.  Miscoded characters are removed.
**
** The translation is done in-place and aborted if the output
** overruns the input.
*/
int __utf8_to8(unsigned char *in){
  unsigned char *out = in;
  unsigned char *start = in;
  u_int32_t c;

  while( in[0] && out<=in ){
    c = __utf8_read((const u_int8_t**)&in);
    if( c!=0xfffd ){
      WRITE_UTF8(out, c);
    }
  }
  *out = 0;
  return (int)(out - start);
}
#endif

#ifndef DBSQL_OMIT_UTF16
/*
** Convert a UTF-16 string in the native encoding into a UTF-8 string.
** Memory to hold the UTF-8 string is obtained from dbsql_malloc and must
** be freed by the calling function.
**
** NULL is returned if there is an allocation error.
*/
char *__utf16to8(DBSQL *db, const void *z, int n_byte, u_int8_t enc){
  mem_t m;
  memset(&m, 0, sizeof(m));
  m.db = db;
  __vdbe_mem_set_str(&m, z, n_byte, enc, DBSQL_STATIC);
  __vdbe_change_encoding(&m, DBSQL_UTF8);
  if( db->malloc_failed ){
    __vdbe_mem_release(&m);
    m.z = 0;
  }
  assert( (m.flags & MEM_Term)!=0 || db->malloc_failed );
  assert( (m.flags & MEM_Str)!=0 || db->malloc_failed );
  assert( m.z || db->malloc_failed );
  return m.z;
}

/*
** in is a UTF-16 encoded unicode string at least n_byte bytes long.
** Return the number of bytes in the first n_char unicode characters
** in z.  n_char must be non-negative.  Surrogate pairs count as a single
** character.
*/
int __utf16_byte_len(const void *in, int n_byte, int n_char){
  int c;
  unsigned char const *z = in;
  unsigned char const *end = &z[n_byte-1];
  int n = 0;
  
  if( DBSQL_UTF16NATIVE==DBSQL_UTF16LE ) z++;
  while( n<n_char && z<=end ){
    c = z[0];
    z += 2;
    if( c>=0xd8 && c<0xdc && z<=end && z[0]>=0xdc && z[0]<0xe0 ) z += 2;
    n++;
  }
  return (int)(z-(unsigned char const *)in) 
              - (DBSQL_UTF16NATIVE==DBSQL_UTF16LE);
}

#if defined(DBSQL_TEST)
/*
** This routine is called from the TCL test function "translate_selftest".
** It checks that the primitives for serializing and deserializing
** characters in each encoding are inverses of each other.
*/
void __utf_self_test(void){
  unsigned int i, t;
  unsigned char buf[20];
  unsigned char *z;
  int n;
  unsigned int c;

  for(i=0; i<0x00110000; i++){
    z = buf;
    WRITE_UTF8(z, i);
    n = (int)(z-buf);
    assert( n>0 && n<=4 );
    z[0] = 0;
    z = buf;
    c = __utf8_read((const u_int8_t**)&z);
    t = i;
    if( i>=0xD800 && i<=0xDFFF ) t = 0xFFFD;
    if( (i&0xFFFFFFFE)==0xFFFE ) t = 0xFFFD;
    assert( c==t );
    assert( (z-buf)==n );
  }
}
#endif /* DBSQL_TEST */
#endif /* DBSQL_OMIT_UTF16 */
