/*
** 2022-11-18
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
** This is a dbsql extension for converting in either direction
** between a (binary) blob and base64 text. Base64 can transit a
** sane USASCII channel unmolested. It also plays nicely in CSV or
** written as TCL brace-enclosed literals or SQL string literals,
** and can be used unmodified in XML-like documents.
**
** This is an independent implementation of conversions specified in
** RFC 4648, done on the above date by the author (Larry Brasfield)
** who thereby has the right to put this into the public domain.
**
** The conversions meet RFC 4648 requirements, provided that this
** C source specifies that line-feeds are included in the encoded
** data to limit visible line lengths to 72 characters and to
** terminate any encoded blob having non-zero length.
**
** Length limitations are not imposed except that the runtime
** dbsql string or blob length limits are respected. Otherwise,
** any length binary sequence can be represented and recovered.
** Generated base64 sequences, with their line-feeds included,
** can be concatenated; the result converted back to binary will
** be the concatenation of the represented binary sequences.
**
** This SQLite3 extension creates a function, base64(x), which
** either: converts text x containing base64 to a returned blob;
** or converts a blob x to returned text containing base64. An
** error will be thrown for other input argument types.
**
** This code relies on UTF-8 encoding only with respect to the
** meaning of the first 128 (7-bit) codes matching that of USASCII.
** It will fail miserably if somehow made to try to convert EBCDIC.
** Because it is table-driven, it could be enhanced to handle that,
** but the world and dbsql have moved on from that anachronism.
**
** To build the extension:
** Set shell variable SQDIR=<your favorite dbsql checkout directory>
** *Nix: gcc -O2 -shared -I$SQDIR -f_pic -o base64.so base64.c
** OSX: gcc -O2 -dynamiclib -f_pic -I$SQDIR -o base64.dylib base64.c
** Win32: gcc -O2 -shared -I%SQDIR% -o base64.dll base64.c
** Win32: cl /Os -I%SQDIR% base64.c -link -dll -out:base64.dll
*/

#include <assert.h>

#include "dbsql_ext.h"

#ifndef deliberate_fall_through
/* Quiet some compilers about some of our intentional code. */
# if GCC_VERSION>=7000000
#  define deliberate_fall_through __attribute__((fallthrough));
# else
#  define deliberate_fall_through
# endif
#endif

DBSQL_EXTENSION_INIT1;

#define PC 0x80 /* pad character */
#define WS 0x81 /* whitespace */
#define ND 0x82 /* Not above or digit-value */
#define PAD_CHAR '='

#ifndef U8_TYPEDEF
typedef unsigned char u_int8_t;
#define U8_TYPEDEF
#endif

/* Decoding table, ASCII (7-bit) value to base 64 digit value or other */
static const u_int8_t __b64_digit_values[128] = {
  /*                             HT LF VT  FF CR       */
    ND,ND,ND,ND, ND,ND,ND,ND, ND,WS,WS,WS, WS,WS,ND,ND,
  /*                                                US */
    ND,ND,ND,ND, ND,ND,ND,ND, ND,ND,ND,ND, ND,ND,ND,ND,
  /*sp                                  +            / */
    WS,ND,ND,ND, ND,ND,ND,ND, ND,ND,ND,62, ND,ND,ND,63,
  /* 0  1            5            9            =       */
    52,53,54,55, 56,57,58,59, 60,61,ND,ND, ND,PC,ND,ND,
  /*    A                                            O */
    ND, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
  /* P                               Z                 */
    15,16,17,18, 19,20,21,22, 23,24,25,ND, ND,ND,ND,ND,
  /*    a                                            o */
    ND,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
  /* p                               z                 */
    41,42,43,44, 45,46,47,48, 49,50,51,ND, ND,ND,ND,ND
};

static const char b64Numerals[64+1]
= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define BX_DV_PROTO(c) \
  ((((u_int8_t)(c))<0x80)? (u_int8_t)(__b64_digit_values[(u_int8_t)(c)]) : 0x80)
#define IS_BX_DIGIT(bdp) (((u_int8_t)(bdp))<0x80)
#define IS_BX_WS(bdp) ((bdp)==WS)
#define IS_BX_PAD(bdp) ((bdp)==PC)
#define BX_NUMERAL(dv) (b64Numerals[(u_int8_t)(dv)])
/* Width of base64 lines. Should be an integer multiple of 4. */
#define B64_DARK_MAX 72

/* Encode a byte buffer into base64 text with linefeeds appended to limit
** encoded group lengths to B64_DARK_MAX or to terminate the last group.
*/
static char* __to_base64( u_int8_t *in, int nb_in, char *out ){
  int n_col = 0;
  while( nb_in >= 3 ){
    /* Do the bit-shuffle, exploiting unsigned input to avoid masking. */
    out[0] = BX_NUMERAL(in[0]>>2);
    out[1] = BX_NUMERAL(((in[0]<<4)|(in[1]>>4))&0x3f);
    out[2] = BX_NUMERAL(((in[1]&0xf)<<2)|(in[2]>>6));
    out[3] = BX_NUMERAL(in[2]&0x3f);
    out += 4;
    nb_in -= 3;
    in += 3;
    if( (n_col += 4)>=B64_DARK_MAX || nb_in<=0 ){
      *out++ = '\n';
      n_col = 0;
    }
  }
  if( nb_in > 0 ){
    signed char nco = nb_in+1;
    int nbe;
    unsigned long qv = *in++;
    for( nbe=1; nbe<3; ++nbe ){
      qv <<= 8;
      if( nbe<nb_in ) qv |= *in++;
    }
    for( nbe=3; nbe>=0; --nbe ){
      char ce = (nbe<nco)? BX_NUMERAL((u_int8_t)(qv & 0x3f)) : PAD_CHAR;
      qv >>= 6;
      out[nbe] = ce;
    }
    out += 4;
    *out++ = '\n';
  }
  *out = 0;
  return out;
}

/* Skip over text which is not base64 numeral(s). */
static char * __skip_non_b64( char *s, int nc ){
  char c;
  while( nc-- > 0 && (c = *s) && !IS_BX_DIGIT(BX_DV_PROTO(c)) ) ++s;
  return s;
}

/* Decode base64 text into a byte buffer. */
static u_int8_t* __from_base64( char *in, int nc_in, u_int8_t *out ){
  if( nc_in>0 && in[nc_in-1]=='\n' ) --nc_in;
  while( nc_in>0 && *in!=PAD_CHAR ){
    static signed char nboi[] = { 0, 0, 1, 2, 3 };
    char *use = __skip_non_b64(in, nc_in);
    unsigned long qv = 0L;
    int nti, nbo, nac;
    nc_in -= (use - in);
    in = use;
    nti = (nc_in>4)? 4 : nc_in;
    nc_in -= nti;
    nbo = nboi[nti];
    if( nbo==0 ) break;
    for( nac=0; nac<4; ++nac ){
      char c = (nac<nti)? *in++ : b64Numerals[0];
      u_int8_t bdp = BX_DV_PROTO(c);
      switch( bdp ){
      case ND:
        /*  Treat dark non-digits as pad, but they terminate decode too. */
        nc_in = 0;
        deliberate_fall_through; /* FALLTHRU */
      case WS:
        /* Treat whitespace as pad and terminate this group.*/
        nti = nac;
        deliberate_fall_through; /* FALLTHRU */
      case PC:
        bdp = 0;
        --nbo;
        deliberate_fall_through; /* FALLTHRU */
      default: /* bdp is the digit value. */
        qv = qv<<6 | bdp;
        break;
      }
    }
    switch( nbo ){
    case 3:
      out[2] = (qv) & 0xff;
      deliberate_fall_through; /* FALLTHRU */
    case 2:
      out[1] = (qv>>8) & 0xff;
      deliberate_fall_through; /* FALLTHRU */
    case 1:
      out[0] = (qv>>16) & 0xff;
      break;
    }
    out += nbo;
  }
  return out;
}

/* This function does the work for the dbsql base64(x) UDF. */
static void base64(dbsql_func_t *context, int na, dbsql_value_t *av[]){
  int64_t nb;
  int64_t nv = dbsql_value_bytes(av[0]);
  int64_t nc;
  int nv_max = dbsql_limit(dbsql_context_db_handle(context),
                            DBSQL_LIMIT_LENGTH, -1);
  char *c_buf;
  u_int8_t *buf;
  assert(na==1);
  switch( dbsql_value_type(av[0]) ){
  case DBSQL_BLOB:
    nb = nv;
    nc = 4*((nv+2)/3); /* quads needed */
    nc += (nc+(B64_DARK_MAX-1))/B64_DARK_MAX + 1; /* LFs and a 0-terminator */
    if( nv_max < nc ){
      dbsql_result_error(context, "blob expanded to base64 too big", -1);
      return;
    }
    buf = (u_int8_t*)dbsql_value_blob(av[0]);
    if( !buf ){
      if( DBSQL_NOMEM==dbsql_errcode(dbsql_context_db_handle(context)) ){
        goto mem_fail;
      }
      dbsql_result_text(context,"",-1,DBSQL_STATIC);
      break;
    }
    c_buf = dbsql_malloc64(nc);
    if( !c_buf ) goto mem_fail;
    nc = (int)(__to_base64(buf, nb, c_buf) - c_buf);
    dbsql_result_text(context, c_buf, nc, dbsql_free);
    break;
  case DBSQL_TEXT:
    nc = nv;
    nb = 3*((nv+3)/4); /* may overestimate due to LF and padding */
    if( nv_max < nb ){
      dbsql_result_error(context, "blob from base64 may be too big", -1);
      return;
    }else if( nb<1 ){
      nb = 1;
    }
    c_buf = (char *)dbsql_value_text(av[0]);
    if( !c_buf ){
      if( DBSQL_NOMEM==dbsql_errcode(dbsql_context_db_handle(context)) ){
        goto mem_fail;
      }
      dbsql_result_zeroblob(context, 0);
      break;
    }
    buf = dbsql_malloc64(nb);
    if( !buf ) goto mem_fail;
    nb = (int)(__from_base64(c_buf, nc, buf) - buf);
    dbsql_result_blob(context, buf, nb, dbsql_free);
    break;
  default:
    dbsql_result_error(context, "base64 accepts only blob or text", -1);
    return;
  }
  return;
 mem_fail:
  dbsql_result_error(context, "base64 OOM", -1);
}

/*
** Establish linkage to running dbsql library.
*/
#ifndef DBSQL_SHELL_EXTFUNCS
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_base64_init
#else
static int dbsql_base64_init
#endif
(DBSQL *db, char **err_p, const dbsql_api_routines_t *api){
  DBSQL_EXTENSION_INIT2(api);
  (void)err_p;
  return dbsql_create_function
    (db, "base64", 1,
     DBSQL_DETERMINISTIC|DBSQL_INNOCUOUS|DBSQL_DIRECTONLY|DBSQL_UTF8,
     0, base64, 0, 0);
}

/*
** Define some macros to allow this extension to be built into the shell
** conveniently, in conjunction with use of DBSQL_SHELL_EXTFUNCS. This
** allows shell.c, as distributed, to have this extension built in.
*/
#define BASE64_INIT(db) dbsql_base64_init(db, 0, 0)
#define BASE64_EXPOSE(db, err_p) /* Not needed, ..._init() does this. */
