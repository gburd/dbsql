/*
** The "printf" code that follows dates from the 1980's.  It is in
** the public domain. 
**
**************************************************************************
**
** This file contains code for a set of "printf"-like routines.  These
** routines format strings much like the printf() from the standard C
** library, though the implementation here has enhancements to support
** dbsql.
*/
#include "dbsql_int.h"

/*
** Conversion types fall into various categories as defined by the
** following enumeration.
*/
#define ET_RADIX       0 /* non-decimal integer types.  %x %o */
#define ET_FLOAT       1 /* Floating point.  %f */
#define ET_EXP         2 /* Exponentional notation. %e and %E */
#define ET_GENERIC     3 /* Floating or exponential, depending on exponent. %g */
#define ET_SIZE        4 /* Return number of characters processed so far. %n */
#define ET_STRING      5 /* Strings. %s */
#define ET_DYNSTRING   6 /* Dynamically allocated strings. %z */
#define ET_PERCENT     7 /* Percent symbol. %% */
#define ET_CHARX       8 /* Characters. %c */
/* The rest are extensions, not normally found in printf() */
#define etESCAPE_q    9  /* Strings with '\'' doubled.  %q */
#define etESCAPE_Q    10 /* Strings with '\'' doubled and enclosed in '',
                            NULL pointers replaced by SQL NULL.  %Q */
#define ET_TOKEN       11 /* a pointer to a Token structure */
#define ET_SRCITEM     12 /* a pointer to a src_item_t */
#define ET_POINTER     13 /* The %p conversion */
#define ET_ESCAPE_W    14 /* %w -> Strings with '\"' doubled */
#define ET_ORDINAL     15 /* %r -> 1st, 2nd, 3rd, 4th, etc.  English only */
#define ET_DECIMAL     16 /* %d or %u, but not %x, %o */

#define ET_INVALID     17 /* Any unrecognized conversion type */


/*
** An "et_byte" is an 8-bit unsigned value.
*/
typedef unsigned char et_byte;

/*
** Each builtin conversion character (ex: the 'd' in "%d") is described
** by an instance of the following structure
*/
typedef struct et_info {   /* Information about each format field */
  char fmttype;            /* The format field code letter */
  et_byte base;             /* The base for radix conversion */
  et_byte flags;            /* One or more of FLAG_ constants below */
  et_byte type;             /* Conversion paradigm */
  et_byte charset;          /* Offset into __a_digits[] of the digits string */
  et_byte prefix;           /* Offset into __a_prefix[] of the prefix string */
  char i_nxt;               /* Next with same hash, or 0 for end of chain */
} et_info;

/*
** Allowed values for et_info.flags
*/
#define FLAG_SIGNED    1     /* True if the value to convert is signed */
#define FLAG_STRING    4     /* Allow infinite precision */

/*
** The table is searched by hash.  In the case of %C where C is the character
** and that character has ASCII value j, then the hash is j%23.
**
** The order of the entries in fmtinfo[] and the hash chain was entered
** manually, but based on the output of the following TCL script:
*/
#if 0  /*****  Beginning of script ******/
foreach c {d s g z q Q w c o u x X f e E G i n % p T S r} {
  scan $c %c x
  set n($c) $x
}
set mx [llength [array names n]]
puts "count: $mx"

set mx 27
puts "*********** mx=$mx ************"
for {set r 0} {$r<$mx} {incr r} {
  puts -nonewline [format %2d: $r]
  foreach c [array names n] {
    if {($n($c))%$mx==$r} {puts -nonewline " $c"}
  }
  puts ""
}
#endif /***** End of script ********/

static const char __a_digits[] = "0123456789ABCDEF0123456789abcdef";
static const char __a_prefix[] = "-x0\000X0";
static const et_info fmtinfo[23] = {
  /*  0 */  {  's',  0, 4, ET_STRING,     0,  0,  1 },
  /*  1 */  {  'E',  0, 1, ET_EXP,        14, 0,  0 },  /* hash_t: 0 */
  /*  2 */  {  'u', 10, 0, ET_DECIMAL,    0,  0,  3 },
  /*  3 */  {  'G',  0, 1, ET_GENERIC,    14, 0,  0 },  /* hash_t: 2 */
  /*  4 */  {  'w',  0, 4, ET_ESCAPE_W,   0,  0,  0 },
  /*  5 */  {  'x', 16, 0, ET_RADIX,      16, 1,  0 },
  /*  6 */  {  'c',  0, 0, ET_CHARX,      0,  0,  0 },  /* hash_t: 7 */
  /*  7 */  {  'z',  0, 4, ET_DYNSTRING,  0,  0,  6 },
  /*  8 */  {  'd', 10, 1, ET_DECIMAL,    0,  0,  0 },
  /*  9 */  {  'e',  0, 1, ET_EXP,        30, 0,  0 },
  /* 10 */  {  'f',  0, 1, ET_FLOAT,      0,  0,  0 },
  /* 11 */  {  'g',  0, 1, ET_GENERIC,    30, 0,  0 },
  /* 12 */  {  'Q',  0, 4, etESCAPE_Q,   0,  0,  0 },
  /* 13 */  {  'i', 10, 1, ET_DECIMAL,    0,  0,  0 },
  /* 14 */  {  '%',  0, 0, ET_PERCENT,    0,  0, 16 },
  /* 15 */  {  'T',  0, 0, ET_TOKEN,      0,  0,  0 },
  /* 16 */  {  'S',  0, 0, ET_SRCITEM,    0,  0,  0 },  /* hash_t: 14 */
  /* 17 */  {  'X', 16, 0, ET_RADIX,      0,  4,  0 },  /* hash_t: 19 */
  /* 18 */  {  'n',  0, 0, ET_SIZE,       0,  0,  0 },
  /* 19 */  {  'o',  8, 0, ET_RADIX,      0,  2, 17 },
  /* 20 */  {  'p', 16, 0, ET_POINTER,    0,  1,  0 },
  /* 21 */  {  'q',  0, 4, etESCAPE_q,   0,  0,  0 },
  /* 22 */  {  'r', 10, 1, ET_ORDINAL,    0,  0,  0 }
};

/* Additional Notes:
**
**    %S    Takes a pointer to src_item_t.  Shows name or database.name
**    %!S   Like %S but prefer the name over the alias
*/

/*
** Set the str_accum_t object to an error mode.
*/
void __str_accum_set_error(str_accum_t *p, u_int8_t error){
  assert( error==DBSQL_NOMEM || error==DBSQL_TOOBIG );
  p->acc_error = error;
  if( p->mx_alloc ) dbsql_str_reset(p);
  if( error==DBSQL_TOOBIG ) __error_to_parser(p->db, error);
}

/*
** Extra argument values from a printf_arguments_t object
*/
static int64_t __get_int_arg(printf_arguments_t *p){
  if( p->n_arg<=p->n_used ) return 0;
  return dbsql_value_int64(p->args[p->n_used++]);
}
static double __get_double_arg(printf_arguments_t *p){
  if( p->n_arg<=p->n_used ) return 0.0;
  return dbsql_value_double(p->args[p->n_used++]);
}
static char *__get_text_arg(printf_arguments_t *p){
  if( p->n_arg<=p->n_used ) return 0;
  return (char*)dbsql_value_text(p->args[p->n_used++]);
}

/*
** Allocate memory for a temporary buffer needed for printf rendering.
**
** If the requested size of the temp buffer is larger than the size
** of the output buffer in accum, then cause an DBSQL_TOOBIG error.
** Do the size check before the memory allocation to prevent rogue
** SQL from requesting large allocations using the precision or width
** field of the printf() function.
*/
static char *__printf_temp_buf(dbsql_str_t *accum, int64_t n){
  char *z;
  if( accum->acc_error ) return 0;
  if( n>accum->n_alloc && n>accum->mx_alloc ){
    __str_accum_set_error(accum, DBSQL_TOOBIG);
    return 0;
  }
  z = dbsql_malloc(n);
  if( z==0 ){
    __str_accum_set_error(accum, DBSQL_NOMEM);
  }
  return z;
}

/*
** On machines with a small stack size, you can redefine the
** DBSQL_PRINT_BUF_SIZE to be something smaller, if desired.
*/
#ifndef DBSQL_PRINT_BUF_SIZE
# define DBSQL_PRINT_BUF_SIZE 70
#endif
#define ET_BUFSIZE DBSQL_PRINT_BUF_SIZE  /* Size of the output buffer */

/*
** Hard limit on the precision of floating-point conversions.
*/
#ifndef DBSQL_PRINTF_PRECISION_LIMIT
# define DBSQL_FP_PRECISION_LIMIT 100000000
#endif

/*
** Render a string given by "fmt" into the str_accum_t object.
*/
void dbsql_str_vappendf(
  dbsql_str_t *accum,       /* Accumulate results here */
  const char *fmt,           /* Format string */
  va_list ap                 /* arguments */
){
  int c;                     /* Next character in the format string */
  char *bufpt;               /* Pointer to the conversion buffer */
  int precision;             /* Precision of the current field */
  int length;                /* Length of the field */
  int idx;                   /* A general purpose loop counter */
  int width;                 /* Width of the current field */
  et_byte flag_leftjustify;   /* True if "-" flag is present */
  et_byte flag_prefix;        /* '+' or ' ' or 0 for prefix */
  et_byte flag_alternateform; /* True if "#" flag is present */
  et_byte flag_altform2;      /* True if "!" flag is present */
  et_byte flag_zeropad;       /* True if field width constant starts with zero */
  et_byte flag_long;          /* 1 for the "l" flag, 2 for "ll", 0 by default */
  et_byte done;               /* Loop termination flag */
  et_byte c_thousand;          /* Thousands separator for %d and %u */
  et_byte xtype = ET_INVALID;  /* Conversion paradigm */
  u_int8_t arg_list;               /* True for DBSQL_PRINTF_SQLFUNC */
  char prefix;               /* Prefix character.  "+" or "-" or " " or '\0'. */
  u_int64_t longvalue;   /* Value for integer types */
  double realvalue;          /* Value for real types */
  const et_info *infop;      /* Pointer to the appropriate info structure */
  char *out;                /* Rendering buffer */
  int n_out;                  /* Size of the rendering buffer */
  char *extra = 0;          /* Malloced memory used by some conversion */
  int exp, e2;               /* exponent of real numbers */
  et_byte flag_dp;            /* True if decimal point should be shown */
  et_byte flag_rtz;           /* True if trailing zeros should be removed */

  printf_arguments_t *p_arg_list = 0; /* Arguments for DBSQL_PRINTF_SQLFUNC */
  char buf[ET_BUFSIZE];       /* Conversion buffer */

  /* accum never starts out with an empty buffer that was obtained from 
  ** malloc().  This precondition is required by the mprintf("%z...")
  ** optimization. */
  assert( accum->n_char>0 || (accum->printf_flags&DBSQL_PRINTF_MALLOCED)==0 );

  bufpt = 0;
  if( (accum->printf_flags & DBSQL_PRINTF_SQLFUNC)!=0 ){
    p_arg_list = va_arg(ap, printf_arguments_t*);
    arg_list = 1;
  }else{
    arg_list = 0;
  }
  for(; (c=(*fmt))!=0; ++fmt){
    if( c!='%' ){
      bufpt = (char *)fmt;
#if HAVE_STRCHRNUL
      fmt = strchrnul(fmt, '%');
#else
      fmt = strchr(fmt, '%');
      if( fmt==0 ){
        fmt = bufpt + strlen(bufpt);
      }
#endif
      dbsql_str_append(accum, bufpt, (int)(fmt - bufpt));
      if( *fmt==0 ) break;
    }
    if( (c=(*++fmt))==0 ){
      dbsql_str_append(accum, "%", 1);
      break;
    }
    /* Find out what flags are present */
    flag_leftjustify = flag_prefix = c_thousand =
     flag_alternateform = flag_altform2 = flag_zeropad = 0;
    done = 0;
    width = 0;
    flag_long = 0;
    precision = -1;
    do{
      switch( c ){
        case '-':   flag_leftjustify = 1;     break;
        case '+':   flag_prefix = '+';        break;
        case ' ':   flag_prefix = ' ';        break;
        case '#':   flag_alternateform = 1;   break;
        case '!':   flag_altform2 = 1;        break;
        case '0':   flag_zeropad = 1;         break;
        case ',':   c_thousand = ',';          break;
        default:    done = 1;                 break;
        case 'l': {
          flag_long = 1;
          c = *++fmt;
          if( c=='l' ){
            c = *++fmt;
            flag_long = 2;
          }
          done = 1;
          break;
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
          unsigned wx = c - '0';
          while( (c = *++fmt)>='0' && c<='9' ){
            wx = wx*10 + c - '0';
          }
          testcase( wx>0x7fffffff );
          width = wx & 0x7fffffff;
#ifdef DBSQL_PRINTF_PRECISION_LIMIT
          if( width>DBSQL_PRINTF_PRECISION_LIMIT ){
            width = DBSQL_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( c!='.' && c!='l' ){
            done = 1;
          }else{
            fmt--;
          }
          break;
        }
        case '*': {
          if( arg_list ){
            width = (int)__get_int_arg(p_arg_list);
          }else{
            width = va_arg(ap,int);
          }
          if( width<0 ){
            flag_leftjustify = 1;
            width = width >= -2147483647 ? -width : 0;
          }
#ifdef DBSQL_PRINTF_PRECISION_LIMIT
          if( width>DBSQL_PRINTF_PRECISION_LIMIT ){
            width = DBSQL_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( (c = fmt[1])!='.' && c!='l' ){
            c = *++fmt;
            done = 1;
          }
          break;
        }
        case '.': {
          c = *++fmt;
          if( c=='*' ){
            if( arg_list ){
              precision = (int)__get_int_arg(p_arg_list);
            }else{
              precision = va_arg(ap,int);
            }
            if( precision<0 ){
              precision = precision >= -2147483647 ? -precision : -1;
            }
            c = *++fmt;
          }else{
            unsigned px = 0;
            while( c>='0' && c<='9' ){
              px = px*10 + c - '0';
              c = *++fmt;
            }
            testcase( px>0x7fffffff );
            precision = px & 0x7fffffff;
          }
#ifdef DBSQL_PRINTF_PRECISION_LIMIT
          if( precision>DBSQL_PRINTF_PRECISION_LIMIT ){
            precision = DBSQL_PRINTF_PRECISION_LIMIT;
          }
#endif
          if( c=='l' ){
            --fmt;
          }else{
            done = 1;
          }
          break;
        }
      }
    }while( !done && (c=(*++fmt))!=0 );

    /* Fetch the info entry for the field */
#ifdef DBSQL_EBCDIC
    /* The hash table only works for ASCII.  For EBCDIC, we need to do
    ** a linear search of the table */
    infop = &fmtinfo[0];
    xtype = ET_INVALID;
    for(idx=0; idx<ARRAY_SIZE(fmtinfo); idx++){
      if( c==fmtinfo[idx].fmttype ){
        infop = &fmtinfo[idx];
        xtype = infop->type;
        break;
      }
    }
#else
    /* Fast hash-table lookup */
    assert( ARRAY_SIZE(fmtinfo)==23 );
    idx = ((unsigned)c) % 23;
    if( fmtinfo[idx].fmttype==c
     || fmtinfo[idx = fmtinfo[idx].i_nxt].fmttype==c
    ){
      infop = &fmtinfo[idx];
      xtype = infop->type;
    }else{
      infop = &fmtinfo[0];
      xtype = ET_INVALID;
    }
#endif

    /*
    ** At this point, variables are initialized as follows:
    **
    **   flag_alternateform          TRUE if a '#' is present.
    **   flag_altform2               TRUE if a '!' is present.
    **   flag_prefix                 '+' or ' ' or zero
    **   flag_leftjustify            TRUE if a '-' is present or if the
    **                               field width was negative.
    **   flag_zeropad                TRUE if the width began with 0.
    **   flag_long                   1 for "l", 2 for "ll"
    **   width                       The specified field width.  This is
    **                               always non-negative.  Zero is the default.
    **   precision                   The specified precision.  The default
    **                               is -1.
    **   xtype                       The class of the conversion.
    **   infop                       Pointer to the appropriate info struct.
    */
    assert( width>=0 );
    assert( precision>=(-1) );
    switch( xtype ){
      case ET_POINTER:
        flag_long = sizeof(char*)==sizeof(int64_t) ? 2 :
                     sizeof(char*)==sizeof(long int) ? 1 : 0;
        /* no break */ deliberate_fall_through
      case ET_ORDINAL:
      case ET_RADIX:      
        c_thousand = 0;
        /* no break */ deliberate_fall_through
      case ET_DECIMAL:
        if( infop->flags & FLAG_SIGNED ){
          int64_t v;
          if( arg_list ){
            v = __get_int_arg(p_arg_list);
          }else if( flag_long ){
            if( flag_long==2 ){
              v = va_arg(ap,int64_t) ;
            }else{
              v = va_arg(ap,long int);
            }
          }else{
            v = va_arg(ap,int);
          }
          if( v<0 ){
            testcase( v==SMALLEST_INT64 );
            testcase( v==(-1) );
            longvalue = ~v;
            longvalue++;
            prefix = '-';
          }else{
            longvalue = v;
            prefix = flag_prefix;
          }
        }else{
          if( arg_list ){
            longvalue = (u_int64_t)__get_int_arg(p_arg_list);
          }else if( flag_long ){
            if( flag_long==2 ){
              longvalue = va_arg(ap,u_int64_t);
            }else{
              longvalue = va_arg(ap,unsigned long int);
            }
          }else{
            longvalue = va_arg(ap,unsigned int);
          }
          prefix = 0;
        }

#if WHERETRACE_ENABLED
        if( xtype==ET_POINTER && __where_trace & 0x100000 ) longvalue = 0;
#endif
#if TREETRACE_ENABLED
        if( xtype==ET_POINTER && __tree_trace & 0x100000 ) longvalue = 0;
#endif

        if( longvalue==0 ) flag_alternateform = 0;
        if( flag_zeropad && precision<width-(prefix!=0) ){
          precision = width-(prefix!=0);
        }
        if( precision<ET_BUFSIZE-10-ET_BUFSIZE/3 ){
          n_out = ET_BUFSIZE;
          out = buf;
        }else{
          u_int64_t n;
          n = (u_int64_t)precision + 10;
          if( c_thousand ) n += precision/3;
          out = extra = __printf_temp_buf(accum, n);
          if( out==0 ) return;
          n_out = (int)n;
        }
        bufpt = &out[n_out-1];
        if( xtype==ET_ORDINAL ){
          static const char ord[] = "thstndrd";
          int x = (int)(longvalue % 10);
          if( x>=4 || (longvalue/10)%10==1 ){
            x = 0;
          }
          *(--bufpt) = ord[x*2+1];
          *(--bufpt) = ord[x*2];
        }
        {
          const char *cset = &__a_digits[infop->charset];
          u_int8_t base = infop->base;
          do{                                           /* Convert to ascii */
            *(--bufpt) = cset[longvalue%base];
            longvalue = longvalue/base;
          }while( longvalue>0 );
        }
        length = (int)(&out[n_out-1]-bufpt);
        if( precision>length ){                         /* zero pad */
          int nn = precision-length;
          bufpt -= nn;
          memset(bufpt,'0',nn);
          length = precision;
        }
        if( c_thousand ){
          int nn = (length - 1)/3;  /* Number of "," to insert */
          int ix = (length - 1)%3 + 1;
          bufpt -= nn;
          for(idx=0; nn>0; idx++){
            bufpt[idx] = bufpt[idx+nn];
            ix--;
            if( ix==0 ){
              bufpt[++idx] = c_thousand;
              nn--;
              ix = 3;
            }
          }
        }
        if( prefix ) *(--bufpt) = prefix;               /* Add sign */
        if( flag_alternateform && infop->prefix ){      /* Add "0" or "0x" */
          const char *pre;
          char x;
          pre = &__a_prefix[infop->prefix];
          for(; (x=(*pre))!=0; pre++) *(--bufpt) = x;
        }
        length = (int)(&out[n_out-1]-bufpt);
        break;
      case ET_FLOAT:
      case ET_EXP:
      case ET_GENERIC: {
        fp_decode_t s;
        int i_round;
        int j;
        int64_t sz_buf_needed;       /* Size needed to hold the output */

        if( arg_list ){
          realvalue = __get_double_arg(p_arg_list);
        }else{
          realvalue = va_arg(ap,double);
        }
        if( precision<0 ) precision = 6;         /* Set default precision */
#ifdef DBSQL_FP_PRECISION_LIMIT
        if( precision>DBSQL_FP_PRECISION_LIMIT ){
          precision = DBSQL_FP_PRECISION_LIMIT;
        }
#endif
        if( xtype==ET_FLOAT ){
          i_round = -precision;
        }else if( xtype==ET_GENERIC ){
          if( precision==0 ) precision = 1;
          i_round = precision;
        }else{
          i_round = precision+1;
        }
        __fp_decode(&s, realvalue, i_round, flag_altform2 ? 20 : 16);
        if( s.is_special ){
          if( s.is_special==2 ){
            bufpt = flag_zeropad ? "null" : "NaN";
            length = __strlen30(bufpt);
            break;
          }else if( flag_zeropad ){
            s.z[0] = '9';
            s.i_dp = 1000;
            s.n = 1;
          }else{
            memcpy(buf, "-Inf", 5);
            bufpt = buf;
            if( s.sign=='-' ){
              /* no-op */
            }else if( flag_prefix ){
              buf[0] = flag_prefix;
            }else{
              bufpt++;
            }
            length = __strlen30(bufpt);
            break;
          }
        }
        if( s.sign=='-' ){
          if( flag_alternateform
           && !flag_prefix
           && xtype==ET_FLOAT
           && s.i_dp<=i_round
          ){
            /* Suppress the minus sign if all of the following are true:
            **   *  The value displayed is zero
            **   *  The '#' flag is used
            **   *  The '+' flag is not used, and
            **   *  The format is %f
            */
            prefix = 0;
          }else{
            prefix = '-';
          }
        }else{
          prefix = flag_prefix;
        }

        exp = s.i_dp-1;

        /*
        ** If the field type is ET_GENERIC, then convert to either ET_EXP
        ** or ET_FLOAT, as appropriate.
        */
        if( xtype==ET_GENERIC ){
          assert( precision>0 );
          precision--;
          flag_rtz = !flag_alternateform;
          if( exp<-4 || exp>precision ){
            xtype = ET_EXP;
          }else{
            precision = precision - exp;
            xtype = ET_FLOAT;
          }
        }else{
          flag_rtz = flag_altform2;
        }
        if( xtype==ET_EXP ){
          e2 = 0;
        }else{
          e2 = s.i_dp - 1;
        }

        sz_buf_needed = MAX(e2,0)+(int64_t)precision+(int64_t)width+10;
        if( c_thousand && e2>0 ) sz_buf_needed += (e2+2)/3;
        if( sz_buf_needed + accum->n_char >= accum->n_alloc ){
          if( accum->mx_alloc==0 && accum->acc_error==0 ){
            /* Unable to allocate space in accum, perhaps because it
            ** is coming from dbsql_snprintf() or similar.  We'll have
            ** to render into temporary space and the memcpy() it over. */
            bufpt = dbsql_malloc(sz_buf_needed);
            if( bufpt==0 ){
              __str_accum_set_error(accum, DBSQL_NOMEM);
              return;
            }
            extra = bufpt;
          }else if( __str_accum_enlarge(accum, sz_buf_needed)<sz_buf_needed ){
            width = length = 0;
            break;
          }else{
            bufpt = accum->text + accum->n_char;
          }
        }else{
          bufpt = accum->text + accum->n_char;
        }
        out = bufpt;

        flag_dp = (precision>0 ?1:0) | flag_alternateform | flag_altform2;
        /* The sign in front of the number */
        if( prefix ){
          *(bufpt++) = prefix;
        }
        /* Digits prior to the decimal point */
        j = 0;
        assert( s.n>0 );
        if( e2<0 ){
          *(bufpt++) = '0';
        }else if( c_thousand ){
          for(; e2>=0; e2--){
            *(bufpt++) = j<s.n ? s.z[j++] : '0';
            if( (e2%3)==0 && e2>1 ) *(bufpt++) = ',';
          }
        }else{
          j = e2+1;
          if( j>s.n ) j = s.n;
          memcpy(bufpt, s.z, j);
          bufpt += j;
          e2 -= j;
          if( e2>=0 ){
            memset(bufpt, '0', e2+1);
            bufpt += e2+1;
            e2 = -1;
          }
        }
        /* The decimal point */
        if( flag_dp ){
          *(bufpt++) = '.';
        }
        /* "0" digits after the decimal point but before the first
        ** significant digit of the number */
        if( e2<(-1) && precision>0 ){
          int nn = -1-e2;
          if( nn>precision ) nn = precision;
          memset(bufpt, '0', nn);
          bufpt += nn;
          precision -= nn;
        }
        /* Significant digits after the decimal point */
        if( precision>0 ){
          int nn = s.n - j;
          if( NEVER(nn>precision) ) nn = precision;
          if( nn>0 ){
            memcpy(bufpt, s.z+j, nn);
            bufpt += nn;
            precision -= nn;
          }
          if( precision>0 && !flag_rtz ){
            memset(bufpt, '0', precision);
            bufpt += precision;
          }
        }
        /* Remove trailing zeros and the "." if no digits follow the "." */
        if( flag_rtz && flag_dp ){
          while( bufpt[-1]=='0' ) *(--bufpt) = 0;
          assert( bufpt>out );
          if( bufpt[-1]=='.' ){
            if( flag_altform2 ){
              *(bufpt++) = '0';
            }else{
              *(--bufpt) = 0;
            }
          }
        }
        /* Add the "nnn" suffix */
        if( xtype==ET_EXP ){
          exp = s.i_dp - 1;
          *(bufpt++) = __a_digits[infop->charset];
          if( exp<0 ){
            *(bufpt++) = '-'; exp = -exp;
          }else{
            *(bufpt++) = '+';
          }
          if( exp>=100 ){
            *(bufpt++) = (char)((exp/100)+'0');        /* 100's digit */
            exp %= 100;
          }
          *(bufpt++) = (char)(exp/10+'0');             /* 10's digit */
          *(bufpt++) = (char)(exp%10+'0');             /* 1's digit */
        }

        length = (int)(bufpt-out);
        assert( length <= sz_buf_needed );
        if( length<width ){
          int64_t n_pad = width - length;
          if( flag_leftjustify ){
            memset(bufpt, ' ', n_pad);
          }else if( !flag_zeropad ){
            memmove(out+n_pad, out, length);
            memset(out, ' ', n_pad);
          }else{
            int adj = prefix!=0;
            memmove(out+n_pad+adj, out+adj, length-adj);
            memset(out+adj, '0', n_pad);
          }
          length = width;
        }

        if( extra==0 ){
          /* The result is being rendered directory into accum.  This
          ** is the command and fast case */
          accum->n_char += length;
          out[length] = 0;
          continue;
        }else{
          /* We were unable to render directly into accum because we
          ** couldn't allocate sufficient memory.  We need to memcpy()
          ** the rendering (or some prefix thereof) into the output
          ** buffer. */
          bufpt[0] = 0;
          bufpt = extra;
          break;
        }
      }
      case ET_SIZE:
        if( !arg_list ){
          *(va_arg(ap,int*)) = accum->n_char;
        }
        length = width = 0;
        break;
      case ET_PERCENT:
        buf[0] = '%';
        bufpt = buf;
        length = 1;
        break;
      case ET_CHARX:
        if( arg_list ){
          bufpt = __get_text_arg(p_arg_list);
          length = 1;
          if( bufpt ){
            buf[0] = c = *(bufpt++);
            if( (c&0xc0)==0xc0 ){
              while( length<4 && (bufpt[0]&0xc0)==0x80 ){
                buf[length++] = *(bufpt++);
              }
            }
          }else{
            buf[0] = 0;
          }
        }else{
          unsigned int ch = va_arg(ap,unsigned int);
          length = __append_one_utf8_character(buf, ch);
        }
        if( precision>1 ){
          int64_t n_prior = 1;
          width -= precision-1;
          if( width>1 && !flag_leftjustify ){
            dbsql_str_appendchar(accum, width-1, ' ');
            width = 0;
          }
          dbsql_str_append(accum, buf, length);
          precision--;
          while( precision > 1 ){
            int64_t n_copy_bytes;
            if( n_prior > precision-1 ) n_prior = precision - 1;
            n_copy_bytes = length*n_prior;
            if( __str_accum_enlarge_if_needed(accum, n_copy_bytes) ){
              break;
            }
            dbsql_str_append(accum,
                 &accum->text[accum->n_char-n_copy_bytes], n_copy_bytes);
            precision -= n_prior;
            n_prior *= 2;
          }
        }
        bufpt = buf;
        flag_altform2 = 1;
        goto adjust_width_for_utf8;
      case ET_STRING:
      case ET_DYNSTRING:
        if( arg_list ){
          bufpt = __get_text_arg(p_arg_list);
          xtype = ET_STRING;
        }else{
          bufpt = va_arg(ap,char*);
        }
        if( bufpt==0 ){
          bufpt = "";
        }else if( xtype==ET_DYNSTRING ){
          if( accum->n_char==0
           && accum->mx_alloc
           && width==0
           && precision<0
           && accum->acc_error==0
          ){
            /* Special optimization for dbsql_mprintf("%z..."):
            ** Extend an existing memory allocation rather than creating
            ** a new one. */
            assert( (accum->printf_flags&DBSQL_PRINTF_MALLOCED)==0 );
            accum->text = bufpt;
            accum->n_alloc = __db_malloc_size(accum->db, bufpt);
            accum->n_char = 0x7fffffff & (int)strlen(bufpt);
            accum->printf_flags |= DBSQL_PRINTF_MALLOCED;
            length = 0;
            break;
          }
          extra = bufpt;
        }
        if( precision>=0 ){
          if( flag_altform2 ){
            /* Set length to the number of bytes needed in order to display
            ** precision characters */
            unsigned char *z = (unsigned char*)bufpt;
            while( precision-- > 0 && z[0] ){
              DBSQL_SKIP_UTF8(z);
            }
            length = (int)(z - (unsigned char*)bufpt);
          }else{
            for(length=0; length<precision && bufpt[length]; length++){}
          }
        }else{
          length = 0x7fffffff & (int)strlen(bufpt);
        }
      adjust_width_for_utf8:
        if( flag_altform2 && width>0 ){
          /* Adjust width to account for extra bytes in UTF-8 characters */
          int ii = length - 1;
          while( ii>=0 ) if( (bufpt[ii--] & 0xc0)==0x80 ) width++;
        }
        break;
      case etESCAPE_q:          /* %q: Escape ' characters */
      case etESCAPE_Q:          /* %Q: Escape ' and enclose in '...' */
      case ET_ESCAPE_W: {        /* %w: Escape " characters */
        int64_t i, j, k, n;
        int need_quote = 0;
        char ch;
        char *escarg;
        char q;

        if( arg_list ){
          escarg = __get_text_arg(p_arg_list);
        }else{
          escarg = va_arg(ap,char*);
        }
        if( escarg==0 ){
          escarg = (xtype==etESCAPE_Q ? "NULL" : "(NULL)");
        }else if( xtype==etESCAPE_Q ){
          need_quote = 1;
        }
        if( xtype==ET_ESCAPE_W ){
          q = '"';
          flag_alternateform = 0;
        }else{
          q = '\'';
        }
        /* For %q, %Q, and %w, the precision is the number of bytes (or
        ** characters if the ! flags is present) to use from the input.
        ** Because of the extra quoting characters inserted, the number
        ** of output characters may be larger than the precision.
        */
        k = precision;
        for(i=n=0; k!=0 && (ch=escarg[i])!=0; i++, k--){
          if( ch==q )  n++;
          if( flag_altform2 && (ch&0xc0)==0xc0 ){
            while( (escarg[i+1]&0xc0)==0x80 ){ i++; }
          }
        }
        if( flag_alternateform ){
          /* For %#q, do unistr()-style backslash escapes for
          ** all control characters, and for backslash itself.
          ** For %#Q, do the same but only if there is at least
          ** one control character. */
          int64_t n_back = 0;
          int64_t n_ctrl = 0;
          for(k=0; k<i; k++){
            if( escarg[k]=='\\' ){
              n_back++;
            }else if( ((u_int8_t*)escarg)[k]<=0x1f ){
              n_ctrl++;
            }
          }
          if( n_ctrl || xtype==etESCAPE_q ){
            n += n_back + 5*n_ctrl;
            if( xtype==etESCAPE_Q ){
              n += 10;
              need_quote = 2;
            }
          }else{
            flag_alternateform = 0;
          }
        }
        n += i + 3;
        if( n>ET_BUFSIZE ){
          bufpt = extra = __printf_temp_buf(accum, n);
          if( bufpt==0 ) return;
        }else{
          bufpt = buf;
        }
        j = 0;
        if( need_quote ){
          if( need_quote==2 ){
            memcpy(&bufpt[j], "unistr('", 8);
            j += 8;
          }else{
            bufpt[j++] = '\'';
          }
        }
        k = i;
        if( flag_alternateform ){
          for(i=0; i<k; i++){
            bufpt[j++] = ch = escarg[i];
            if( ch==q ){
              bufpt[j++] = ch;
            }else if( ch=='\\' ){
              bufpt[j++] = '\\';
            }else if( ((unsigned char)ch)<=0x1f ){
              bufpt[j-1] = '\\';
              bufpt[j++] = 'u';
              bufpt[j++] = '0';
              bufpt[j++] = '0';
              bufpt[j++] = ch>=0x10 ? '1' : '0';
              bufpt[j++] = "0123456789abcdef"[ch&0xf];
            }
          }
        }else{
          for(i=0; i<k; i++){
            bufpt[j++] = ch = escarg[i];
            if( ch==q ) bufpt[j++] = ch;
          }
        }
        if( need_quote ){
          bufpt[j++] = '\'';
          if( need_quote==2 ) bufpt[j++] = ')';
        }
        bufpt[j] = 0;
        length = j;
        goto adjust_width_for_utf8;
      }
      case ET_TOKEN: {
        if( (accum->printf_flags & DBSQL_PRINTF_INTERNAL)==0 ) return;
        if( flag_alternateform ){
          /* %#T means an expr_t pointer that uses expr_t.u.token */
          expr_t *expr = va_arg(ap,expr_t*);
          if( ALWAYS(expr) && ALWAYS(!EXPR_HAS_PROPERTY(expr,EP_IntValue)) ){
            dbsql_str_appendall(accum, (const char*)expr->u.token);
            __record_error_offset_of_expr(accum->db, expr);
          }
        }else{
          /* %T means a Token pointer */
          Token *token = va_arg(ap, Token*);
          assert( arg_list==0 );
          if( token && token->n ){
            dbsql_str_append(accum, (const char*)token->z, token->n);
            __record_error_byte_offset(accum->db, token->z);
          }
        }
        length = width = 0;
        break;
      }
      case ET_SRCITEM: {
        src_item_t *item;
        if( (accum->printf_flags & DBSQL_PRINTF_INTERNAL)==0 ) return;
        item = va_arg(ap, src_item_t*);
        assert( arg_list==0 );
        if( item->alias && !flag_altform2 ){
          dbsql_str_appendall(accum, item->alias);
        }else if( item->name ){
          if( item->fg.fixed_schema==0
           && item->fg.is_subquery==0
           && item->u4.database!=0
          ){
            dbsql_str_appendall(accum, item->u4.database);
            dbsql_str_append(accum, ".", 1);
          }
          dbsql_str_appendall(accum, item->name);
        }else if( item->alias ){
          dbsql_str_appendall(accum, item->alias);
        }else if( ALWAYS(item->fg.is_subquery) ){/* Because of tag-20240424-1 */
          select_t *sel = item->u4.subq->p_select;
          assert( sel!=0 ); 
          if( sel->sel_flags & SF_NestedFrom ){
            dbsql_str_appendf(accum, "(join-%u)", sel->sel_id);
          }else if( sel->sel_flags & SF_MultiValue ){
            assert( !item->fg.is_tab_func && !item->fg.is_indexed_by );
            dbsql_str_appendf(accum, "%u-ROW VALUES CLAUSE",
                                item->u1.n_row);
          }else{
            dbsql_str_appendf(accum, "(subquery-%u)", sel->sel_id);
          }
        }
        length = width = 0;
        break;
      }
      default: {
        assert( xtype==ET_INVALID );
        return;
      }
    }/* End switch over the format type */
    /*
    ** The text of the conversion is pointed to by "bufpt" and is
    ** "length" characters long.  The field width is "width".  Do
    ** the output.  Both length and width are in bytes, not characters,
    ** at this point.  If the "!" flag was present on string conversions
    ** indicating that width and precision should be expressed in characters,
    ** then the values have been translated prior to reaching this point.
    */
    width -= length;
    if( width>0 ){
      if( !flag_leftjustify ) dbsql_str_appendchar(accum, width, ' ');
      dbsql_str_append(accum, bufpt, length);
      if( flag_leftjustify ) dbsql_str_appendchar(accum, width, ' ');
    }else{
      dbsql_str_append(accum, bufpt, length);
    }

    if( extra ){
      __dbsql_db_free(accum->db, extra);
      extra = 0;
    }
  }/* End for loop over the format string */
} /* End of function */


/*
** The z string points to the first character of a token that is
** associated with an error.  If db does not already have an error
** byte offset recorded, try to compute the error byte offset for
** z and set the error byte offset in db.
*/
void __record_error_byte_offset(DBSQL *db, const char *z){
  const parser_t *parser;
  const char *text;
  const char *end;
  assert( z!=0 );
  if( NEVER(db==0) ) return;
  if( db->err_byte_offset!=(-2) ) return;
  parser = db->parser;
  if( NEVER(parser==0) ) return;
  text =parser->tail;
  if( NEVER(text==0) ) return;
  end = &text[strlen(text)];
  if( DBSQL_WITHIN(z,text,end) ){
    db->err_byte_offset = (int)(z-text);
  }
}

/*
** If expr has a byte offset for the start of a token, record that as
** as the error offset.
*/
void __record_error_offset_of_expr(DBSQL *db, const expr_t *expr){
  while( expr
     && (EXPR_HAS_PROPERTY(expr,EP_OuterON|EP_InnerON) || expr->w.i_ofst<=0)
  ){
    expr = expr->left;
  }
  if( expr==0 ) return;
  if( EXPR_HAS_PROPERTY(expr, EP_FromDDL) ) return;
  db->err_byte_offset = expr->w.i_ofst;
}

/*
** Enlarge the memory allocation on a str_accum_t object so that it is
** able to accept at least N more bytes of text.
**
** Return the number of bytes of text that str_accum_t is able to accept
** after the attempted enlargement.  The value returned might be zero.
*/
int __str_accum_enlarge(str_accum_t *p, int64_t N){
  char *z_new;
  assert( p->n_char+N >= p->n_alloc ); /* Only called if really needed */
  if( p->acc_error ){
    testcase(p->acc_error==DBSQL_TOOBIG);
    testcase(p->acc_error==DBSQL_NOMEM);
    return 0;
  }
  if( p->mx_alloc==0 ){
    __str_accum_set_error(p, DBSQL_TOOBIG);
    return p->n_alloc - p->n_char - 1;
  }else{
    char *old = IS_MALLOCED(p) ? p->text : 0;
    int64_t sz_new = p->n_char + N + 1;
    if( sz_new+p->n_char<=p->mx_alloc ){
      /* Force exponential buffer size growth as long as it does not overflow,
      ** to avoid having to call this routine too often */
      sz_new += p->n_char;
    }
    if( sz_new > p->mx_alloc ){
      dbsql_str_reset(p);
      __str_accum_set_error(p, DBSQL_TOOBIG);
      return 0;
    }else{
      p->n_alloc = (int)sz_new;
    }
    if( p->db ){
      z_new = __db_realloc(p->db, old, p->n_alloc);
    }else{
      z_new = __dbsql_realloc(old, p->n_alloc);
    }
    if( z_new ){
      assert( p->text!=0 || p->n_char==0 );
      if( !IS_MALLOCED(p) && p->n_char>0 ) memcpy(z_new, p->text, p->n_char);
      p->text = z_new;
      p->n_alloc = __db_malloc_size(p->db, z_new);
      p->printf_flags |= DBSQL_PRINTF_MALLOCED;
    }else{
      dbsql_str_reset(p);
      __str_accum_set_error(p, DBSQL_NOMEM);
      return 0;
    }
  }
  assert( N>=0 && N<=0x7fffffff );
  return (int)N;
}

int __str_accum_enlarge_if_needed(str_accum_t *p, int64_t N){
  if( N + p->n_char >= p->n_alloc ){
    __str_accum_enlarge(p, N);
  }
  return p->acc_error;
}

/*
** Append N copies of character c to the given string buffer.
*/
void dbsql_str_appendchar(dbsql_str_t *p, int N, char c){
  testcase( p->n_char + (int64_t)N > 0x7fffffff );
  if( p->n_char+(int64_t)N >= p->n_alloc && (N = __str_accum_enlarge(p, N))<=0 ){
    return;
  }
  while( (N--)>0 ) p->text[p->n_char++] = c;
}

/*
** The str_accum_t "p" is not large enough to accept N new bytes of z[].
** So enlarge if first, then do the append.
**
** This is a helper routine to dbsql_str_append() that does special-case
** work (enlarging the buffer) using tail recursion, so that the
** dbsql_str_append() routine can use fast calling semantics.
*/
static void DBSQL_NOINLINE __enlarge_and_append(str_accum_t *p, const char *z, int N){
  N = __str_accum_enlarge(p, N);
  if( N>0 ){
    memcpy(&p->text[p->n_char], z, N);
    p->n_char += N;
  }
}

/*
** Append N bytes of text from z to the str_accum_t object.  Increase the
** size of the memory allocation for str_accum_t if necessary.
*/
void dbsql_str_append(dbsql_str_t *p, const char *z, int N){
  assert( z!=0 || N==0 );
  assert( p->text!=0 || p->n_char==0 || p->acc_error );
  assert( N>=0 );
  assert( p->acc_error==0 || p->n_alloc==0 || p->mx_alloc==0 );
  if( p->n_char+N >= p->n_alloc ){
    __enlarge_and_append(p,z,N);
  }else if( N ){
    assert( p->text );
    p->n_char += N;
    memcpy(&p->text[p->n_char-N], z, N);
  }
}

/*
** Append the complete text of zero-terminated string z[] to the p string.
*/
void dbsql_str_appendall(dbsql_str_t *p, const char *z){
  dbsql_str_append(p, z, __strlen30(z));
}


/*
** Finish off a string by making sure it is zero-terminated.
** Return a pointer to the resulting string.  Return a NULL
** pointer if any kind of error was encountered.
*/
static DBSQL_NOINLINE char *__str_accum_finish_realloc(str_accum_t *p){
  char *text;
  assert( p->mx_alloc>0 && !IS_MALLOCED(p) );
  text = __db_malloc_raw(p->db, 1+(u_int64_t)p->n_char );
  if( text ){
    memcpy(text, p->text, p->n_char+1);
    p->printf_flags |= DBSQL_PRINTF_MALLOCED;
  }else{
    __str_accum_set_error(p, DBSQL_NOMEM);
  }
  p->text = text;
  return text;
}
char *__str_accum_finish(str_accum_t *p){
  if( p->text ){
    p->text[p->n_char] = 0;
    if( p->mx_alloc>0 && !IS_MALLOCED(p) ){
      return __str_accum_finish_realloc(p);
    }
  }
  return p->text;
}

/*
** Use the content of the str_accum_t passed as the second argument
** as the result of an SQL function.
*/
void __result_str_accum(dbsql_func_t *ctx, str_accum_t *p){
  if( p->acc_error ){
    dbsql_result_error_code(ctx, p->acc_error);
    dbsql_str_reset(p);
  }else if( IS_MALLOCED(p) ){
    dbsql_result_text(ctx, p->text, p->n_char, DBSQL_DYNAMIC);
  }else{
    dbsql_result_text(ctx, "", 0, DBSQL_STATIC);
    dbsql_str_reset(p);
  }
}

/*
** This singleton is an dbsql_str_t object that is returned if
** dbsql_malloc() fails to provide space for a real one.  This
** dbsql_str_t object accepts no new text and always returns
** an DBSQL_NOMEM error.
*/
static dbsql_str_t __oom_str = {
   0, 0, 0, 0, 0, DBSQL_NOMEM, 0
};

/* Finalize a string created using dbsql_str_new().
*/
char *dbsql_str_finish(dbsql_str_t *p){
  char *z;
  if( p!=0 && p!=&__oom_str ){
    z = __str_accum_finish(p);
    dbsql_free(p);
  }else{
    z = 0;
  }
  return z;
}

/* Return any error code associated with p */
int dbsql_str_errcode(dbsql_str_t *p){
  return p ? p->acc_error : DBSQL_NOMEM;
}

/* Return the current length of p in bytes */
int dbsql_str_length(dbsql_str_t *p){
  return p ? p->n_char : 0;
}

/* Truncate the text of the string to be no more than N bytes. */
void dbsql_str_truncate(dbsql_str_t *p, int N){
  if( p!=0 && N>=0 && (u_int32_t)N<p->n_char ){
    p->n_char = N;
    p->text[p->n_char] = 0;
  }
}

/* Return the current value for p */
char *dbsql_str_value(dbsql_str_t *p){
  if( p==0 || p->n_char==0 ) return 0;
  p->text[p->n_char] = 0;
  return p->text;
}

/*
** Reset an str_accum_t string.  Reclaim all malloced memory.
*/
void dbsql_str_reset(str_accum_t *p){
  if( IS_MALLOCED(p) ){
    __dbsql_db_free(p->db, p->text);
    p->printf_flags &= ~DBSQL_PRINTF_MALLOCED;
  }
  p->n_alloc = 0;
  p->n_char = 0;
  p->text = 0;
}

/*
** Destroy a dynamically allocate dbsql_str_t object and all
** of its content, all in one call.
*/
void dbsql_str_free(dbsql_str_t *p){
  if( p!=0 && p!=&__oom_str ){
    dbsql_str_reset(p);
    dbsql_free(p);
  }
}

/*
** Initialize a string accumulator.
**
** p:     The accumulator to be initialized.
** db:    Pointer to a database connection.  May be NULL.  lookaside_t
**        memory is used if not NULL. db->malloc_failed is set appropriately
**        when not NULL.
** base: An initial buffer.  May be NULL in which case the initial buffer
**        is malloced.
** n:     Size of base in bytes.  If total space requirements never exceed
**        n then no memory allocations ever occur.
** mx:    Maximum number of bytes to accumulate.  If mx==0 then no memory
**        allocations will ever occur.
*/
void __str_accum_init(str_accum_t *p, DBSQL *db, char *base, int n, int mx){
  p->text = base;
  p->db = db;
  p->n_alloc = n;
  p->mx_alloc = mx;
  p->n_char = 0;
  p->acc_error = 0;
  p->printf_flags = 0;
}

/* Allocate and initialize a new dynamic string object */
dbsql_str_t *dbsql_str_new(DBSQL *db){
  dbsql_str_t *p = dbsql_malloc64(sizeof(*p));
  if( p ){
    __str_accum_init(p, 0, 0, 0,
            db ? db->limits[DBSQL_LIMIT_LENGTH] : DBSQL_MAX_LENGTH);
  }else{
    p = &__oom_str;
  }
  return p;
}

/*
** Print into memory obtained from dbsql_malloc().  Use the internal
** %-conversion extensions.
*/
char *__vm_printf(DBSQL *db, const char *format, va_list ap){
  char *z;
  char base[DBSQL_PRINT_BUF_SIZE];
  str_accum_t acc;
  assert( db!=0 );
  __str_accum_init(&acc, db, base, sizeof(base),
                      db->limits[DBSQL_LIMIT_LENGTH]);
  acc.printf_flags = DBSQL_PRINTF_INTERNAL;
  dbsql_str_vappendf(&acc, format, ap);
  z = __str_accum_finish(&acc);
  if( acc.acc_error==DBSQL_NOMEM ){
    __oom_fault(db);
  }
  return z;
}

/*
** Print into memory obtained from dbsql_malloc().  Use the internal
** %-conversion extensions.
*/
char *__m_printf(DBSQL *db, const char *format, ...){
  va_list ap;
  char *z;
  va_start(ap, format);
  z = __vm_printf(db, format, ap);
  va_end(ap);
  return z;
}

/*
** Print into memory obtained from dbsql_malloc().  Omit the internal
** %-conversion extensions.
*/
char *dbsql_vmprintf(const char *format, va_list ap){
  char *z;
  char base[DBSQL_PRINT_BUF_SIZE];
  str_accum_t acc;

#ifdef DBSQL_ENABLE_API_ARMOR  
  if( format==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  __str_accum_init(&acc, 0, base, sizeof(base), DBSQL_MAX_LENGTH);
  dbsql_str_vappendf(&acc, format, ap);
  z = __str_accum_finish(&acc);
  return z;
}

/*
** Print into memory obtained from dbsql_malloc()().  Omit the internal
** %-conversion extensions.
*/
char *dbsql_mprintf(const char *format, ...){
  va_list ap;
  char *z;
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  va_start(ap, format);
  z = dbsql_vmprintf(format, ap);
  va_end(ap);
  return z;
}

/*
** dbsql_snprintf() works like snprintf() except that it ignores the
** current locale settings.  This is important for dbsql because we
** are not able to use a "," as the decimal point in place of "." as
** specified by some locales.
**
** Oops:  The first two arguments of dbsql_snprintf() are backwards
** from the snprintf() standard.  Unfortunately, it is too late to change
** this without breaking compatibility, so we just have to live with the
** mistake.
**
** dbsql_vsnprintf() is the varargs version.
*/
char *dbsql_vsnprintf(int n, char *buf, const char *format, va_list ap){
  str_accum_t acc;
  if( n<=0 ) return buf;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( buf==0 || format==0 ) {
    (void)DBSQL_MISUSE_BKPT;
    if( buf ) buf[0] = 0;
    return buf;
  }
#endif
  __str_accum_init(&acc, 0, buf, n, 0);
  dbsql_str_vappendf(&acc, format, ap);
  buf[acc.n_char] = 0;
  return buf;
}
char *dbsql_snprintf(int n, char *buf, const char *format, ...){
  str_accum_t acc;
  va_list ap;
  if( n<=0 ) return buf;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( buf==0 || format==0 ) {
    (void)DBSQL_MISUSE_BKPT;
    if( buf ) buf[0] = 0;
    return buf;
  }
#endif
  __str_accum_init(&acc, 0, buf, n, 0);
  va_start(ap,format);
  dbsql_str_vappendf(&acc, format, ap);
  va_end(ap);
  buf[acc.n_char] = 0;
  return buf;
}

/* Maximum size of an dbsql_log() message. */
#if defined(DBSQL_MAX_LOG_MESSAGE) 
  /* Leave the definition as supplied */
#elif DBSQL_PRINT_BUF_SIZE*10>10000
# define DBSQL_MAX_LOG_MESSAGE 10000
#else
# define DBSQL_MAX_LOG_MESSAGE (DBSQL_PRINT_BUF_SIZE*10)
#endif

/*
** This is the routine that actually formats the dbsql_log() message.
** We house it in a separate routine from dbsql_log() to avoid using
** stack space on small-stack systems when logging is disabled.
**
** dbsql_log() must render into a static buffer.  It cannot dynamically
** allocate memory because it might be called while the memory allocator
** mutex is held.
**
** dbsql_str_vappendf() might ask for *temporary* memory allocations for
** certain format characters (%q) or for very large precisions or widths.
** Care must be taken that any dbsql_log() calls that occur while the
** memory mutex is held do not use these mechanisms.
*/
static void __render_log_msg(int i_err_code, const char *format, va_list ap){
  str_accum_t acc;                          /* String accumulator */
  char msg[DBSQL_MAX_LOG_MESSAGE];     /* Complete log message */

  __str_accum_init(&acc, 0, msg, sizeof(msg), 0);
  dbsql_str_vappendf(&acc, format, ap);
  __global_config.x_log(__global_config.log_arg, i_err_code,
                           __str_accum_finish(&acc));
}

/*
** Format and write a message to the log if logging is enabled.
*/
void dbsql_log(int i_err_code, const char *format, ...){
  va_list ap;                             /* Vararg list */
  if( __global_config.x_log ){
    va_start(ap, format);
    __render_log_msg(i_err_code, format, ap);
    va_end(ap);
  }
}

#if defined(DBSQL_DEBUG) || defined(DBSQL_HAVE_OS_TRACE)
/*
** A version of printf() that understands %lld.  Used for debugging.
** The printf() built into some versions of windows does not understand %lld
** and segfaults if you give it a long long int.
*/
void __debug_printf(const char *format, ...){
  va_list ap;
  str_accum_t acc;
  char buf[DBSQL_PRINT_BUF_SIZE*10];
  __str_accum_init(&acc, 0, buf, sizeof(buf), 0);
  va_start(ap,format);
  dbsql_str_vappendf(&acc, format, ap);
  va_end(ap);
  __str_accum_finish(&acc);
#ifdef DBSQL_OS_TRACE_PROC
  {
    extern void DBSQL_OS_TRACE_PROC(const char *buf, int n_buf);
    DBSQL_OS_TRACE_PROC(buf, sizeof(buf));
  }
#else
  fprintf(stdout,"%s", buf);
  fflush(stdout);
#endif
}
#endif


/*
** variable-argument wrapper around dbsql_str_vappendf(). The flags argument
** can contain the bit DBSQL_PRINTF_INTERNAL enable internal formats.
*/
void dbsql_str_appendf(str_accum_t *p, const char *format, ...){
  va_list ap;
  va_start(ap,format);
  dbsql_str_vappendf(p, format, ap);
  va_end(ap);
}


/*****************************************************************************
** Reference counted string/blob storage
*****************************************************************************/

/*
** Increase the reference count of the string by one.
**
** The input parameter is returned.
*/
char *__rc_str_ref(char *z){
  rc_str_t *p = (rc_str_t*)z;
  assert( p!=0 );
  p--;
  p->n_rc_ref++;
  return z;
}

/*
** Decrease the reference count by one.  Free the string when the
** reference count reaches zero.
*/
void __rc_str_unref(void *z){
  rc_str_t *p = (rc_str_t*)z;
  assert( p!=0 );
  p--;
  assert( p->n_rc_ref>0 );
  if( p->n_rc_ref>=2 ){
    p->n_rc_ref--;
  }else{
    dbsql_free(p);
  }
}

/*
** Create a new string that is capable of holding N bytes of text, not counting
** the zero byte at the end.  The string is uninitialized.
**
** The reference count is initially 1.  Call __rc_str_unref() to free the
** newly allocated string.
**
** This routine returns 0 on an OOM.
*/
char *__rc_str_new(u_int64_t N){
  rc_str_t *p = dbsql_malloc64( N + sizeof(*p) + 1 );
  if( p==0 ) return 0;
  p->n_rc_ref = 1;
  return (char*)&p[1];
}

/*
** Change the size of the string so that it is able to hold N bytes.
** The string might be reallocated, so return the new allocation.
*/
char *__rc_str_resize(char *z, u_int64_t N){
  rc_str_t *p = (rc_str_t*)z;
  rc_str_t *p_new;
  assert( p!=0 );
  p--;
  assert( p->n_rc_ref==1 );
  p_new = dbsql_realloc64(p, N+sizeof(rc_str_t)+1);
  if( p_new==0 ){
    dbsql_free(p);
    return 0;
  }else{
    return (char*)&p_new[1];
  }
}
