/*
** 2003 October 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C functions that implement date and time
** functions for dbsql.  
**
** There is only one exported symbol in this file - the function
** __register_date_time_functions() found at the bottom of the file.
** All other code has file scope.
**
** dbsql processes all times and dates as julian day numbers.  The
** dates and times are stored as the number of days since noon
** in Greenwich on November 24, 4714 B.C. according to the Gregorian
** calendar system. 
**
** 1970-01-01 00:00:00 is JD 2440587.5
** 2000-01-01 00:00:00 is JD 2451544.5
**
** This implementation requires years to be expressed as a 4-digit number
** which means that only dates between 0000-01-01 and 9999-12-31 can
** be represented, even though julian day numbers allow a much wider
** range of dates.
**
** The Gregorian calendar system is used for all dates and times,
** even those that predate the Gregorian calendar.  Historians usually
** use the julian calendar for dates prior to 1582-10-15 and for some
** dates afterwards, depending on locale.  Beware of this difference.
**
** The conversion algorithms are implemented based on descriptions
** in the following text:
**
**      Jean Meeus
**      Astronomical Algorithms, 2nd Edition, 1998
**      ISBN 0-943396-61-1
**      Willmann-Bell, Inc
**      Richmond, Virginia (USA)
*/
#include "dbsql_int.h"
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#ifndef DBSQL_OMIT_DATETIME_FUNCS

/*
** The MSVC CRT on Windows CE may not have a localtime() function.
** So declare a substitute.  The substitute function itself is
** defined in "os_win.c".
*/
#if !defined(DBSQL_OMIT_LOCALTIME) && defined(_WIN32_WCE) && \
    (!defined(DBSQL_MSVC_LOCALTIME_API) || !DBSQL_MSVC_LOCALTIME_API)
struct tm *__cdecl localtime(const time_t *);
#endif

/*
** A structure for holding a single date and time.
*/
typedef struct date_time_t date_time_t;
struct date_time_t {
  int64_t i_jd;  /* The julian day number times 86400000 */
  int Y, M, D;        /* Year, month, and day */
  int h, m;           /* Hour and minutes */
  int tz;             /* Timezone offset in minutes */
  double s;           /* Seconds */
  char valid_jd;       /* True (1) if i_jd is valid */
  char valid_ymd;      /* True (1) if Y,M,D are valid */
  char valid_hms;      /* True (1) if h,m,s are valid */
  char n_floor;            /* Days to implement "floor" */
  unsigned raw_s      : 1; /* Raw numeric value stored in s */
  unsigned isError   : 1; /* An overflow has occurred */
  unsigned use_subsec : 1; /* Display subsecond precision */
  unsigned is_utc     : 1; /* Time is known to be UTC */
  unsigned isLocal   : 1; /* Time is known to be localtime */
};


/*
** Convert date into one or more integers according to the conversion
** specifier format.
**
** format[] contains 4 characters for each integer converted, except for
** the last integer which is specified by three characters.  The meaning
** of a four-character format specifiers ABCD is:
**
**    A:   number of digits to convert.  Always "2" or "4".
**    B:   minimum value.  Always "0" or "1".
**    C:   maximum value, decoded as:
**           a:  12
**           b:  14
**           c:  24
**           d:  31
**           e:  59
**           f:  9999
**    D:   the separator character, or \000 to indicate this is the
**         last number to convert.
**
** Example:  To translate an ISO-8601 date YYYY-MM-DD, the format would
** be "40f-21a-20c".  The "40f-" indicates the 4-digit year followed by "-".
** The "21a-" indicates the 2-digit month followed by "-".  The "20c" indicates
** the 2-digit day which is the last integer in the set.
**
** The function returns the number of successful conversions.
*/
static int __get_digits(const char *date, const char *format, ...){
  /* The mxes[] array translates the 3rd character of each format
  ** spec into a max size:    a   b   c   d   e      f */
  static const u_int16_t mxes[] = { 12, 14, 24, 31, 59, 14712 };
  va_list ap;
  int cnt = 0;
  char next_c;
  va_start(ap, format);
  do{
    char N = format[0] - '0';
    char min = format[1] - '0';
    int val = 0;
    u_int16_t max;

    assert( format[2]>='a' && format[2]<='f' );
    max = mxes[format[2] - 'a'];
    next_c = format[3];
    val = 0;
    while( N-- ){
      if( !__isdigit(*date) ){
        goto end_get_digits;
      }
      val = val*10 + *date - '0';
      date++;
    }
    if( val<(int)min || val>(int)max || (next_c!=0 && next_c!=*date) ){
      goto end_get_digits;
    }
    *va_arg(ap,int*) = val;
    date++;
    cnt++;
    format += 4;
  }while( next_c );
end_get_digits:
  va_end(ap);
  return cnt;
}

/*
** parser_t a timezone extension on the end of a date-time.
** The extension is of the form:
**
**        (+/-)HH:MM
**
** Or the "zulu" notation:
**
**        Z
**
** If the parse is successful, write the number of minutes
** of change in p->tz and return 0.  If a parser error occurs,
** return non-zero.
**
** A missing specifier is not considered an error.
*/
static int __parse_timezone(const char *date, date_time_t *p){
  int sgn = 0;
  int n_hr, n_mn;
  int c;
  while( __isspace(*date) ){ date++; }
  p->tz = 0;
  c = *date;
  if( c=='-' ){
    sgn = -1;
  }else if( c=='+' ){
    sgn = +1;
  }else if( c=='Z' || c=='z' ){
    date++;
    p->isLocal = 0;
    p->is_utc = 1;
    goto zulu_time;
  }else{
    return c!=0;
  }
  date++;
  if( __get_digits(date, "20b:20e", &n_hr, &n_mn)!=2 ){
    return 1;
  }
  date += 5;
  p->tz = sgn*(n_mn + n_hr*60);
  if( p->tz==0 ){   /* Forum post 2025-09-17T10:12:14z */
    p->isLocal = 0;
    p->is_utc = 1;
  }
zulu_time:
  while( __isspace(*date) ){ date++; }
  return *date!=0;
}

/*
** parser_t times of the form HH:MM or HH:MM:SS or HH:MM:SS.FFFF.
** The HH, MM, and SS must each be exactly 2 digits.  The
** fractional seconds FFFF can be one or more digits.
**
** Return 1 if there is a parsing error and 0 on success.
*/
static int __parse_hh_mm_ss(const char *date, date_time_t *p){
  int h, m, s;
  double ms = 0.0;
  if( __get_digits(date, "20c:20e", &h, &m)!=2 ){
    return 1;
  }
  date += 5;
  if( *date==':' ){
    date++;
    if( __get_digits(date, "20e", &s)!=1 ){
      return 1;
    }
    date += 2;
    if( *date=='.' && __isdigit(date[1]) ){
      double r_scale = 1.0;
      date++;
      while( __isdigit(*date) ){
        ms = ms*10.0 + *date - '0';
        r_scale *= 10.0;
        date++;
      }
      ms /= r_scale;
      /* Truncate to avoid problems with sub-milliseconds
      ** rounding. https://sqlite.org/forum/forumpost/766a2c9231 */
      if( ms>0.999 ) ms = 0.999;
    }
  }else{
    s = 0;
  }
  p->valid_jd = 0;
  p->raw_s = 0;
  p->valid_hms = 1;
  p->h = h;
  p->m = m;
  p->s = s + ms;
  if( __parse_timezone(date, p) ) return 1;
  return 0;
}

/*
** Put the date_time_t object into its error state.
*/
static void __datetime_error(date_time_t *p){
  memset(p, 0, sizeof(*p));
  p->isError = 1;
}

/*
** Convert from YYYY-MM-DD HH:MM:SS to julian day.  We always assume
** that the YYYY-MM-DD is according to the Gregorian calendar.
**
** Reference:  Meeus page 61
*/
static void __compute_jd(date_time_t *p){
  int Y, M, D, A, B, X1, X2;

  if( p->valid_jd ) return;
  if( p->valid_ymd ){
    Y = p->Y;
    M = p->M;
    D = p->D;
  }else{
    Y = 2000;  /* If no YMD specified, assume 2000-Jan-01 */
    M = 1;
    D = 1;
  }
  if( Y<-4713 || Y>9999 || p->raw_s ){
    __datetime_error(p);
    return;
  }
  if( M<=2 ){
    Y--;
    M += 12;
  }
  A = (Y+4800)/100;
  B = 38 - A + (A/4);
  X1 = 36525*(Y+4716)/100;
  X2 = 306001*(M+1)/10000;
  p->i_jd = (int64_t)((X1 + X2 + D + B - 1524.5 ) * 86400000);
  p->valid_jd = 1;
  if( p->valid_hms ){
    p->i_jd += p->h*3600000 + p->m*60000 + (int64_t)(p->s*1000 + 0.5);
    if( p->tz ){
      p->i_jd -= p->tz*60000;
      p->valid_ymd = 0;
      p->valid_hms = 0;
      p->tz = 0;
      p->is_utc = 1;
      p->isLocal = 0;
    }
  }
}

/*
** Given the YYYY-MM-DD information current in p, determine if there
** is day-of-month overflow and set n_floor to the number of days that
** would need to be subtracted from the date in order to bring the
** date back to the end of the month.
*/
static void __compute_floor(date_time_t *p){
  assert( p->valid_ymd || p->isError );
  assert( p->D>=0 && p->D<=31 );
  assert( p->M>=0 && p->M<=12 );
  if( p->D<=28 ){
    p->n_floor = 0;
  }else if( (1<<p->M) & 0x15aa ){
    p->n_floor = 0;
  }else if( p->M!=2 ){
    p->n_floor = (p->D==31);
  }else if( p->Y%4!=0 || (p->Y%100==0 && p->Y%400!=0) ){
    p->n_floor = p->D - 28;
  }else{
    p->n_floor = p->D - 29;
  }
}

/*
** parser_t dates of the form
**
**     YYYY-MM-DD HH:MM:SS.FFF
**     YYYY-MM-DD HH:MM:SS
**     YYYY-MM-DD HH:MM
**     YYYY-MM-DD
**
** Write the result into the date_time_t structure and return 0
** on success and 1 if the input string is not a well-formed
** date.
*/
static int __parse_yyyy_mm_dd(const char *date, date_time_t *p){
  int Y, M, D, neg;

  if( date[0]=='-' ){
    date++;
    neg = 1;
  }else{
    neg = 0;
  }
  if( __get_digits(date, "40f-21a-21d", &Y, &M, &D)!=3 ){
    return 1;
  }
  date += 10;
  while( __isspace(*date) || 'T'==*(u_int8_t*)date ){ date++; }
  if( __parse_hh_mm_ss(date, p)==0 ){
    /* We got the time */
  }else if( *date==0 ){
    p->valid_hms = 0;
  }else{
    return 1;
  }
  p->valid_jd = 0;
  p->valid_ymd = 1;
  p->Y = neg ? -Y : Y;
  p->M = M;
  p->D = D;
  __compute_floor(p);
  if( p->tz ){
    __compute_jd(p);
  }
  return 0;
}


static void __clear_ymd_hms_tz(date_time_t *p);  /* Forward declaration */

/*
** Set the time to the current time reported by the VFS.
**
** Return the number of errors.
*/
static int __set_date_time_to_current(dbsql_func_t *context, date_time_t *p){
  p->i_jd = __stmt_current_time(context);
  if( p->i_jd>0 ){
    p->valid_jd = 1;
    p->is_utc = 1;
    p->isLocal = 0;
    __clear_ymd_hms_tz(p);
    return 0;
  }else{
    return 1;
  }
}

/*
** Input "r" is a numeric quantity which might be a julian day number,
** or the number of seconds since 1970.  If the value if r is within
** range of a julian day number, install it as such and set valid_jd.
** If the value is a valid unix timestamp, put it in p->s and set p->raw_s.
*/
static void __set_raw_date_number(date_time_t *p, double r){
  p->s = r;
  p->raw_s = 1;
  if( r>=0.0 && r<5373484.5 ){
    p->i_jd = (int64_t)(r*86400000.0 + 0.5);
    p->valid_jd = 1;
  }
}

/*
** Attempt to parse the given string into a julian day number.  Return
** the number of errors.
**
** The following are acceptable forms for the input string:
**
**      YYYY-MM-DD HH:MM:SS.FFF  +/-HH:MM
**      DDDD.DD 
**      now
**
** In the first form, the +/-HH:MM is always optional.  The fractional
** seconds extension (the ".FFF") is optional.  The seconds portion
** (":SS.FFF") is option.  The year and date can be omitted as long
** as there is a time string.  The time string can be omitted as long
** as there is a year and date.
*/
static int __parse_date_or_time(
  dbsql_func_t *context, 
  const char *date, 
  date_time_t *p
){
  double r;
  if( __parse_yyyy_mm_dd(date,p)==0 ){
    return 0;
  }else if( __parse_hh_mm_ss(date, p)==0 ){
    return 0;
  }else if( __str_icmp(date,"now")==0 && __not_pure_func(context) ){
    return __set_date_time_to_current(context, p);
  }else if( __ato_f(date, &r)>0 ){
    __set_raw_date_number(p, r);
    return 0;
  }else if( (__str_icmp(date,"subsec")==0
             || __str_icmp(date,"subsecond")==0)
           && __not_pure_func(context) ){
    p->use_subsec = 1;
    return __set_date_time_to_current(context, p);
  }
  return 1;
}

/* The julian day number for 9999-12-31 23:59:59.999 is 5373484.4999999.
** Multiplying this by 86400000 gives 464269060799999 as the maximum value
** for date_time_t.i_jd.
**
** But some older compilers (ex: gcc 4.2.1 on older Macs) cannot deal with 
** such a large integer literal, so we have to encode it.
*/
#define INT_464269060799999  ((((int64_t)0x1a640)<<32)|0x1072fdff)

/*
** Return TRUE if the given julian day number is within range.
**
** The input is the __julian_day times 86400000.
*/
static int __valid_julian_day(int64_t i_jd){
  return i_jd>=0 && i_jd<=INT_464269060799999;
}

/*
** Compute the Year, Month, and Day from the julian day number.
*/
static void __compute_ymd(date_time_t *p){
  int Z, alpha, A, B, C, D, E, X1;
  if( p->valid_ymd ) return;
  if( !p->valid_jd ){
    p->Y = 2000;
    p->M = 1;
    p->D = 1;
  }else if( !__valid_julian_day(p->i_jd) ){
    __datetime_error(p);
    return;
  }else{
    Z = (int)((p->i_jd + 43200000)/86400000);
    alpha = (int)((Z + 32044.75)/36524.25) - 52;
    A = Z + 1 + alpha - ((alpha+100)/4) + 25;
    B = A + 1524;
    C = (int)((B - 122.1)/365.25);
    D = (36525*(C&32767))/100;
    E = (int)((B-D)/30.6001);
    X1 = (int)(30.6001*E);
    p->D = B - D - X1;
    p->M = E<14 ? E-1 : E-13;
    p->Y = p->M>2 ? C - 4716 : C - 4715;
  }
  p->valid_ymd = 1;
}

/*
** Compute the Hour, Minute, and Seconds from the julian day number.
*/
static void __compute_hms(date_time_t *p){
  int day_ms, day_min; /* milliseconds, minutes into the day */
  if( p->valid_hms ) return;
  __compute_jd(p);
  day_ms = (int)((p->i_jd + 43200000) % 86400000);
  p->s = (day_ms % 60000)/1000.0;
  day_min = day_ms/60000;
  p->m = day_min % 60;
  p->h = day_min / 60;
  p->raw_s = 0;
  p->valid_hms = 1;
}

/*
** Compute both YMD and HMS
*/
static void __compute_ymd_hms(date_time_t *p){
  __compute_ymd(p);
  __compute_hms(p);
}

/*
** Clear the YMD and HMS and the TZ
*/
static void __clear_ymd_hms_tz(date_time_t *p){
  p->valid_ymd = 0;
  p->valid_hms = 0;
  p->tz = 0;
}

#ifndef DBSQL_OMIT_LOCALTIME
/*
** On recent Windows platforms, the localtime_s() function is available
** as part of the "Secure CRT". It is essentially equivalent to 
** localtime_r() available under most POSIX platforms, except that the 
** order of the parameters is reversed.
**
** See http://msdn.microsoft.com/en-us/library/a442x3ye(VS.80).aspx.
**
** If the user has not indicated to use localtime_r() or localtime_s()
** already, check for an MSVC build environment that provides 
** localtime_s().
*/
#if !HAVE_LOCALTIME_R && !HAVE_LOCALTIME_S \
    && defined(_MSC_VER) && defined(_CRT_INSECURE_DEPRECATE)
#undef  HAVE_LOCALTIME_S
#define HAVE_LOCALTIME_S 1
#endif

/*
** The following routine implements the rough equivalent of localtime_r()
** using whatever operating-system specific localtime facility that
** is available.  This routine returns 0 on success and
** non-zero on any kind of error.
**
** If the __global_config.localtime_fault variable is non-zero then this
** routine will always fail.  If localtime_fault is nonzero and
** __global_config.alt_localtime is not NULL, then alt_localtime() is
** invoked in place of the OS-defined localtime() function.
**
** EVIDENCE-OF: R-62172-00036 In this implementation, the standard C
** library function localtime_r() is used to assist in the calculation of
** local time.
*/
static int __os_localtime(time_t *t, struct tm *p_tm){
  int rc;
#if !HAVE_LOCALTIME_R && !HAVE_LOCALTIME_S
  struct tm *x;
#if DBSQL_THREADSAFE>0
  dbsql_mutex_t *mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);
#endif
  dbsql_mutex_enter(mutex);
  x = localtime(t);
#ifndef DBSQL_UNTESTABLE
  if( __global_config.localtime_fault ){
    if( __global_config.alt_localtime!=0
     && 0==__global_config.alt_localtime((const void*)t,(void*)p_tm)
    ){
      x = p_tm;
    }else{
      x = 0;
    }
  }
#endif
  if( x ) *p_tm = *x;
#if DBSQL_THREADSAFE>0
  dbsql_mutex_leave(mutex);
#endif
  rc = x==0;
#else
#ifndef DBSQL_UNTESTABLE
  if( __global_config.localtime_fault ){
    if( __global_config.alt_localtime!=0 ){
      return __global_config.alt_localtime((const void*)t,(void*)p_tm);
    }else{
      return 1;
    }
  }
#endif
#if HAVE_LOCALTIME_R
  rc = localtime_r(t, p_tm)==0;
#else
  rc = localtime_s(p_tm, t);
#endif /* HAVE_LOCALTIME_R */
#endif /* HAVE_LOCALTIME_R || HAVE_LOCALTIME_S */
  return rc;
}
#endif /* DBSQL_OMIT_LOCALTIME */


#ifndef DBSQL_OMIT_LOCALTIME
/*
** Assuming the input date_time_t is UTC, move it to its localtime equivalent.
*/
static int __to_localtime(
  date_time_t *p,                   /* Date at which to calculate offset */
  dbsql_func_t *ctx          /* Write error here if one occurs */
){
  time_t t;
  struct tm s_local;
  int i_year_diff;

  /* Initialize the contents of s_local to avoid a compiler warning. */
  memset(&s_local, 0, sizeof(s_local));

  __compute_jd(p);
  if( p->i_jd<2108667600*(int64_t)100000 /* 1970-01-01 */
   || p->i_jd>2130141456*(int64_t)100000 /* 2038-01-18 */
  ){
    /* EVIDENCE-OF: R-55269-29598 The localtime_r() C function normally only
    ** works for years between 1970 and 2037. For dates outside this range,
    ** dbsql attempts to map the year into an equivalent year within this
    ** range, do the calculation, then map the year back.
    */
    date_time_t x = *p;
    __compute_ymd_hms(&x);
    i_year_diff = (2000 + x.Y%4) - x.Y;
    x.Y += i_year_diff;
    x.valid_jd = 0;
    __compute_jd(&x);
    t = (time_t)(x.i_jd/1000 -  21086676*(int64_t)10000);
  }else{
    i_year_diff = 0;
    t = (time_t)(p->i_jd/1000 -  21086676*(int64_t)10000);
  }
  if( __os_localtime(&t, &s_local) ){
    dbsql_result_error(ctx, "local time unavailable", -1);
    return DBSQL_ERROR;
  }
  p->Y = s_local.tm_year + 1900 - i_year_diff;
  p->M = s_local.tm_mon + 1;
  p->D = s_local.tm_mday;
  p->h = s_local.tm_hour;
  p->m = s_local.tm_min;
  p->s = s_local.tm_sec + (p->i_jd%1000)*0.001;
  p->valid_ymd = 1;
  p->valid_hms = 1;
  p->valid_jd = 0;
  p->raw_s = 0;
  p->tz = 0;
  p->isError = 0;
  return DBSQL_OK;
}
#endif /* DBSQL_OMIT_LOCALTIME */

/*
** The following table defines various date transformations of the form
**
**            'NNN days'
**
** Where NNN is an arbitrary floating-point number and "days" can be one
** of several units of time.
*/
static const struct {
  u_int8_t n_name;           /* Length of the name */
  char name[7];      /* Name of the transformation */
  float r_limit;       /* Maximum NNN value for this transform */
  float r_xform;       /* Constant used for this transform */
} __a_xform_type[] = {
  /* 0 */ { 6, "second",   4.6427e+14,         1.0  },
  /* 1 */ { 6, "minute",   7.7379e+12,        60.0  },
  /* 2 */ { 4, "hour",     1.2897e+11,      3600.0  },
  /* 3 */ { 3, "day",      5373485.0,      86400.0  },
  /* 4 */ { 5, "month",    176546.0,     2592000.0  },
  /* 5 */ { 4, "year",     14713.0,     31536000.0  },
};

/*
** If the date_time_t p is raw number, try to figure out if it is
** a julian day number of a unix timestamp.  Set the p value
** appropriately.
*/
static void __auto_adjust_date(date_time_t *p){
  if( !p->raw_s || p->valid_jd ){
    p->raw_s = 0;
  }else if( p->s>=-21086676*(int64_t)10000        /* -4713-11-24 12:00:00 */
         && p->s<=(25340230*(int64_t)10000)+799   /*  9999-12-31 23:59:59 */
  ){
    double r = p->s*1000.0 + 210866760000000.0;
    __clear_ymd_hms_tz(p);
    p->i_jd = (int64_t)(r + 0.5);
    p->valid_jd = 1;
    p->raw_s = 0;
  }
}

/*
** Process a modifier to a date-time stamp.  The modifiers are
** as follows:
**
**     NNN days
**     NNN hours
**     NNN minutes
**     NNN.NNNN seconds
**     NNN months
**     NNN years
**     +/-YYYY-MM-DD HH:MM:SS.SSS
**     ceiling
**     floor
**     start of month
**     start of year
**     start of week
**     start of day
**     weekday N
**     unixepoch
**     auto
**     localtime
**     utc
**     subsec
**     subsecond
**
** Return 0 on success and 1 if there is any kind of error. If the error
** is in a system call (i.e. localtime()), then an error message is written
** to context ctx. If the error is an unrecognized modifier, no error is
** written to ctx.
*/
static int __parse_modifier(
  dbsql_func_t *ctx,      /* Function context */
  const char *z,              /* The text of the modifier */
  int n,                      /* Length of mod in bytes */
  date_time_t *p,                /* The date/time value to be modified */
  int idx                     /* Parameter index of the modifier */
){
  int rc = 1;
  double r;
  switch(__upper_to_lower[(u_int8_t)z[0]] ){
    case 'a': {
      /*
      **    auto
      **
      ** If raw_s is available, then interpret as a julian day number, or
      ** a unix timestamp, depending on its magnitude.
      */
      if( dbsql_stricmp(z, "auto")==0 ){
        if( idx>1 ) return 1; /* IMP: R-33611-57934 */
        __auto_adjust_date(p);
        rc = 0;
      }
      break;
    }
    case 'c': {
      /*
      **    ceiling
      **
      ** Resolve day-of-month overflow by rolling forward into the next
      ** month.  As this is the default action, this modifier is really
      ** a no-op that is only included for symmetry.  See "floor".
      */
      if( dbsql_stricmp(z, "ceiling")==0 ){
        __compute_jd(p);
        __clear_ymd_hms_tz(p);
        rc = 0;
        p->n_floor = 0;
      }
      break;
    }
    case 'f': {
      /*
      **    floor
      **
      ** Resolve day-of-month overflow by rolling back to the end of the
      ** previous month.
      */
      if( dbsql_stricmp(z, "floor")==0 ){
        __compute_jd(p);
        p->i_jd -= p->n_floor*86400000;
        __clear_ymd_hms_tz(p);
        rc = 0;
      }
      break;
    }
    case 'j': {
      /*
      **    julianday
      **
      ** Always interpret the prior number as a julian-day value.  If this
      ** is not the first modifier, or if the prior argument is not a numeric
      ** value in the allowed range of julian day numbers understood by
      ** dbsql (0..5373484.5) then the result will be NULL.
      */
      if( dbsql_stricmp(z, "julianday")==0 ){
        if( idx>1 ) return 1;  /* IMP: R-31176-64601 */
        if( p->valid_jd && p->raw_s ){
          rc = 0;
          p->raw_s = 0;
        }
      }
      break;
    }
#ifndef DBSQL_OMIT_LOCALTIME
    case 'l': {
      /*    localtime
      **
      ** Assuming the current time value is UTC (a.k.a. GMT), shift it to
      ** show local time.
      */
      if( dbsql_stricmp(z, "localtime")==0 && __not_pure_func(ctx) ){
        rc = p->isLocal ? DBSQL_OK : __to_localtime(p, ctx);
        p->is_utc = 0;
        p->isLocal = 1;
      }
      break;
    }
#endif
    case 'u': {
      /*
      **    unixepoch
      **
      ** Treat the current value of p->s as the number of
      ** seconds since 1970.  Convert to a real julian day number.
      */
      if( dbsql_stricmp(z, "unixepoch")==0 && p->raw_s ){
        if( idx>1 ) return 1;  /* IMP: R-49255-55373 */
        r = p->s*1000.0 + 210866760000000.0;
        if( r>=0.0 && r<464269060800000.0 ){
          __clear_ymd_hms_tz(p);
          p->i_jd = (int64_t)(r + 0.5);
          p->valid_jd = 1;
          p->raw_s = 0;
          rc = 0;
        }
      }
#ifndef DBSQL_OMIT_LOCALTIME
      else if( dbsql_stricmp(z, "utc")==0 && __not_pure_func(ctx) ){
        if( p->is_utc==0 ){
          int64_t i_orig_jd;              /* Original localtime */
          int64_t i_guess;               /* Guess at the corresponding utc time */
          int cnt = 0;              /* Safety to prevent infinite loop */
          int64_t i_err;                 /* Guess is off by this much */

          __compute_jd(p);
          i_guess = i_orig_jd = p->i_jd;
          i_err = 0;
          do{
            date_time_t new;
            memset(&new, 0, sizeof(new));
            i_guess -= i_err;
            new.i_jd = i_guess;
            new.valid_jd = 1;
            rc = __to_localtime(&new, ctx);
            if( rc ) return rc;
            __compute_jd(&new);
            i_err = new.i_jd - i_orig_jd;
          }while( i_err && cnt++<3 );
          memset(p, 0, sizeof(*p));
          p->i_jd = i_guess;
          p->valid_jd = 1;
          p->is_utc = 1;
          p->isLocal = 0;
        }
        rc = DBSQL_OK;
      }
#endif
      break;
    }
    case 'w': {
      /*
      **    weekday N
      **
      ** Move the date to the same time on the next occurrence of
      ** weekday N where 0==Sunday, 1==Monday, and so forth.  If the
      ** date is already on the appropriate weekday, this is a no-op.
      */
      if( dbsql_strnicmp(z, "weekday ", 8)==0
               && __ato_f(&z[8], &r)>0
               && r>=0.0 && r<7.0 && (n=(int)r)==r ){
        int64_t Z;
        __compute_ymd_hms(p);
        p->tz = 0;
        p->valid_jd = 0;
        __compute_jd(p);
        Z = ((p->i_jd + 129600000)/86400000) % 7;
        if( Z>n ) Z -= 7;
        p->i_jd += (n - Z)*86400000;
        __clear_ymd_hms_tz(p);
        rc = 0;
      }
      break;
    }
    case 's': {
      /*
      **    start of TTTTT
      **
      ** Move the date backwards to the beginning of the current day,
      ** or month or year.
      **
      **    subsecond
      **    subsec
      **
      ** Show subsecond precision in the output of datetime() and
      ** unixepoch() and strftime('%s').
      */
      if( dbsql_strnicmp(z, "start of ", 9)!=0 ){
        if( dbsql_stricmp(z, "subsec")==0
         || dbsql_stricmp(z, "subsecond")==0
        ){
          p->use_subsec = 1;
          rc = 0;
        }
        break;
      }        
      if( !p->valid_jd && !p->valid_ymd && !p->valid_hms ) break;
      z += 9;
      __compute_ymd(p);
      p->valid_hms = 1;
      p->h = p->m = 0;
      p->s = 0.0;
      p->raw_s = 0;
      p->tz = 0;
      p->valid_jd = 0;
      if( dbsql_stricmp(z,"month")==0 ){
        p->D = 1;
        rc = 0;
      }else if( dbsql_stricmp(z,"year")==0 ){
        p->M = 1;
        p->D = 1;
        rc = 0;
      }else if( dbsql_stricmp(z,"day")==0 ){
        rc = 0;
      }
      break;
    }
    case '+':
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      double r_rounder;
      int i, rx;
      int Y,M,D,h,m,x;
      const char *z2 = z;
      char *copy;
      DBSQL *db = dbsql_context_db_handle(ctx);
      char z0 = z[0];
      for(n=1; z[n]; n++){
        if( z[n]==':' ) break;
        if( __isspace(z[n]) ) break;
        if( z[n]=='-' ){
          if( n==5 && __get_digits(&z[1], "40f", &Y)==1 ) break;
          if( n==6 && __get_digits(&z[1], "50f", &Y)==1 ) break;
        }
      }
      copy = __db_str_n_dup(db, z, n);
      if( copy==0 ) break;
      rx = __ato_f(copy, &r)<=0;
      __dbsql_db_free(db, copy);
      if( rx ){
        assert( rc==1 );
        break;
      }
      if( z[n]=='-' ){
        /* A modifier of the form (+|-)YYYY-MM-DD adds or subtracts the
        ** specified number of years, months, and days.  MM is limited to
        ** the range 0-11 and DD is limited to 0-30.
        */
        if( z0!='+' && z0!='-' ) break;  /* Must start with +/- */
        if( n==5 ){
          if( __get_digits(&z[1], "40f-20a-20d", &Y, &M, &D)!=3 ) break;
        }else{
          assert( n==6 );
          if( __get_digits(&z[1], "50f-20a-20d", &Y, &M, &D)!=3 ) break;
          z++;
        }
        if( M>=12 ) break;                   /* M range 0..11 */
        if( D>=31 ) break;                   /* D range 0..30 */
        __compute_ymd_hms(p);
        p->valid_jd = 0;
        if( z0=='-' ){
          p->Y -= Y;
          p->M -= M;
          D = -D;
        }else{
          p->Y += Y;
          p->M += M;
        }
        x = p->M>0 ? (p->M-1)/12 : (p->M-12)/12;
        p->Y += x;
        p->M -= x*12;
        __compute_floor(p);
        __compute_jd(p);
        p->valid_hms = 0;
        p->valid_ymd = 0;
        p->i_jd += (int64_t)D*86400000;
        if( z[11]==0 ){
          rc = 0;
          break;
        }
        if( __isspace(z[11])
         && __get_digits(&z[12], "20c:20e", &h, &m)==2
        ){
          z2 = &z[12];
          n = 2;
        }else{
          break;
        }
      }
      if( z2[n]==':' ){
        /* A modifier of the form (+|-)HH:MM:SS.FFF adds (or subtracts) the
        ** specified number of hours, minutes, seconds, and fractional seconds
        ** to the time.  The ".FFF" may be omitted.  The ":SS.FFF" may be
        ** omitted.
        */

        date_time_t tx;
        int64_t day;
        if( !__isdigit(*z2) ) z2++;
        memset(&tx, 0, sizeof(tx));
        if( __parse_hh_mm_ss(z2, &tx) ) break;
        __compute_jd(&tx);
        tx.i_jd -= 43200000;
        day = tx.i_jd/86400000;
        tx.i_jd -= day*86400000;
        if( z0=='-' ) tx.i_jd = -tx.i_jd;
        __compute_jd(p);
        __clear_ymd_hms_tz(p);
        p->i_jd += tx.i_jd;
        rc = 0;
        break;
      }

      /* If control reaches this point, it means the transformation is
      ** one of the forms like "+NNN days".  */
      z += n;
      while( __isspace(*z) ) z++;
      n = __strlen30(z);
      if( n<3 || n>10 ) break;
      if( __upper_to_lower[(u_int8_t)z[n-1]]=='s' ) n--;
      __compute_jd(p);
      assert( rc==1 );
      r_rounder = r<0 ? -0.5 : +0.5;
      p->n_floor = 0;
      for(i=0; i<ARRAY_SIZE(__a_xform_type); i++){
        if( __a_xform_type[i].n_name==n
         && dbsql_strnicmp(__a_xform_type[i].name, z, n)==0
         && r>-__a_xform_type[i].r_limit && r<__a_xform_type[i].r_limit
        ){
          switch( i ){
            case 4: { /* Special processing to add months */
              assert( strcmp(__a_xform_type[4].name,"month")==0 );
              __compute_ymd_hms(p);
              p->M += (int)r;
              x = p->M>0 ? (p->M-1)/12 : (p->M-12)/12;
              p->Y += x;
              p->M -= x*12;
              __compute_floor(p);
              p->valid_jd = 0;
              r -= (int)r;
              break;
            }
            case 5: { /* Special processing to add years */
              int y = (int)r;
              assert( strcmp(__a_xform_type[5].name,"year")==0 );
              __compute_ymd_hms(p);
              assert( p->M>=0 && p->M<=12 );
              p->Y += y;
              __compute_floor(p);
              p->valid_jd = 0;
              r -= (int)r;
              break;
            }
          }
          __compute_jd(p);
          p->i_jd += (int64_t)(r*1000.0*__a_xform_type[i].r_xform + r_rounder);
          rc = 0;
          break;
        }
      }
      __clear_ymd_hms_tz(p);
      break;
    }
    default: {
      break;
    }
  }
  return rc;
}

/*
** Process time function arguments.  argv[0] is a date-time stamp.
** argv[1] and following are modifiers.  parser_t them all and write
** the resulting time into the date_time_t structure p.  Return 0
** on success and 1 if there are any errors.
**
** If there are zero parameters (if even argv[0] is undefined)
** then assume a default value of "now" for argv[0].
*/
static int __is_date(
  dbsql_func_t *context, 
  int argc, 
  dbsql_value_t **argv, 
  date_time_t *p
){
  int i, n;
  const unsigned char *z;
  int type;
  memset(p, 0, sizeof(*p));
  if( argc==0 ){
    if( !__not_pure_func(context) ) return 1;
    return __set_date_time_to_current(context, p);
  }
  if( (type = dbsql_value_type(argv[0]))==DBSQL_FLOAT
                   || type==DBSQL_INTEGER ){
    __set_raw_date_number(p, dbsql_value_double(argv[0]));
  }else{
    z = dbsql_value_text(argv[0]);
    if( !z || __parse_date_or_time(context, (char*)z, p) ){
      return 1;
    }
  }
  for(i=1; i<argc; i++){
    z = dbsql_value_text(argv[i]);
    n = dbsql_value_bytes(argv[i]);
    if( z==0 || __parse_modifier(context, (char*)z, n, p, i) ) return 1;
  }
  __compute_jd(p);
  if( p->isError || !__valid_julian_day(p->i_jd) ) return 1;
  if( argc==1 && p->valid_ymd && p->D>28 ){
    /* Make sure a YYYY-MM-DD is normalized.
    ** Example: 2023-02-31 -> 2023-03-03 */
    assert( p->valid_jd );
    p->valid_ymd = 0;  
  }
  return 0;
}


/*
** The following routines implement the various date and time functions
** of dbsql.
*/

/*
**    julianday( TIMESTRING, MOD, MOD, ...)
**
** Return the julian day number of the date specified in the arguments
*/
static void __julianday_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    __compute_jd(&x);
    dbsql_result_double(context, x.i_jd/86400000.0);
  }
}

/*
**    unixepoch( TIMESTRING, MOD, MOD, ...)
**
** Return the number of seconds (including fractional seconds) since
** the unix epoch of 1970-01-01 00:00:00 GMT.
*/
static void __unixepoch_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    __compute_jd(&x);
    if( x.use_subsec ){
      dbsql_result_double(context, (x.i_jd - 21086676*(int64_t)10000000)/1000.0);
    }else{
      dbsql_result_int64(context, x.i_jd/1000 - 21086676*(int64_t)10000);
    }
  }
}

/*
**    datetime( TIMESTRING, MOD, MOD, ...)
**
** Return YYYY-MM-DD HH:MM:SS
*/
static void __datetime_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    int Y, s, n;
    char buf[32];
    __compute_ymd_hms(&x);
    Y = x.Y;
    if( Y<0 ) Y = -Y;
    buf[1] = '0' + (Y/1000)%10;
    buf[2] = '0' + (Y/100)%10;
    buf[3] = '0' + (Y/10)%10;
    buf[4] = '0' + (Y)%10;
    buf[5] = '-';
    buf[6] = '0' + (x.M/10)%10;
    buf[7] = '0' + (x.M)%10;
    buf[8] = '-';
    buf[9] = '0' + (x.D/10)%10;
    buf[10] = '0' + (x.D)%10;
    buf[11] = ' ';
    buf[12] = '0' + (x.h/10)%10;
    buf[13] = '0' + (x.h)%10;
    buf[14] = ':';
    buf[15] = '0' + (x.m/10)%10;
    buf[16] = '0' + (x.m)%10;
    buf[17] = ':';
    if( x.use_subsec ){
      s = (int)(1000.0*x.s + 0.5);
      buf[18] = '0' + (s/10000)%10;
      buf[19] = '0' + (s/1000)%10;
      buf[20] = '.';
      buf[21] = '0' + (s/100)%10;
      buf[22] = '0' + (s/10)%10;
      buf[23] = '0' + (s)%10;
      buf[24] = 0;
      n = 24;
    }else{
      s = (int)x.s;
      buf[18] = '0' + (s/10)%10;
      buf[19] = '0' + (s)%10;
      buf[20] = 0;
      n = 20;
    }
    if( x.Y<0 ){
      buf[0] = '-';
      dbsql_result_text(context, buf, n, DBSQL_TRANSIENT);
    }else{
      dbsql_result_text(context, &buf[1], n-1, DBSQL_TRANSIENT);
    }
  }
}

/*
**    time( TIMESTRING, MOD, MOD, ...)
**
** Return HH:MM:SS
*/
static void __time_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    int s, n;
    char buf[16];
    __compute_hms(&x);
    buf[0] = '0' + (x.h/10)%10;
    buf[1] = '0' + (x.h)%10;
    buf[2] = ':';
    buf[3] = '0' + (x.m/10)%10;
    buf[4] = '0' + (x.m)%10;
    buf[5] = ':';
    if( x.use_subsec ){
      s = (int)(1000.0*x.s + 0.5);
      buf[6] = '0' + (s/10000)%10;
      buf[7] = '0' + (s/1000)%10;
      buf[8] = '.';
      buf[9] = '0' + (s/100)%10;
      buf[10] = '0' + (s/10)%10;
      buf[11] = '0' + (s)%10;
      buf[12] = 0;
      n = 12;
    }else{
      s = (int)x.s;
      buf[6] = '0' + (s/10)%10;
      buf[7] = '0' + (s)%10;
      buf[8] = 0;
      n = 8;
    }
    dbsql_result_text(context, buf, n, DBSQL_TRANSIENT);
  }
}

/*
**    date( TIMESTRING, MOD, MOD, ...)
**
** Return YYYY-MM-DD
*/
static void __date_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    int Y;
    char buf[16];
    __compute_ymd(&x);
    Y = x.Y;
    if( Y<0 ) Y = -Y;
    buf[1] = '0' + (Y/1000)%10;
    buf[2] = '0' + (Y/100)%10;
    buf[3] = '0' + (Y/10)%10;
    buf[4] = '0' + (Y)%10;
    buf[5] = '-';
    buf[6] = '0' + (x.M/10)%10;
    buf[7] = '0' + (x.M)%10;
    buf[8] = '-';
    buf[9] = '0' + (x.D/10)%10;
    buf[10] = '0' + (x.D)%10;
    buf[11] = 0;
    if( x.Y<0 ){
      buf[0] = '-';
      dbsql_result_text(context, buf, 11, DBSQL_TRANSIENT);
    }else{
      dbsql_result_text(context, &buf[1], 10, DBSQL_TRANSIENT);
    }
  }
}

/*
** Compute the number of days after the most recent January 1.
**
** In other words, compute the zero-based day number for the
** current year:
**
**   Jan01 = 0,  Jan02 = 1, ..., Jan31 = 30, Feb01 = 31, ...
**   Dec31 = 364 or 365.
*/
static int __days_after_jan01(date_time_t *date){
  date_time_t jan01 = *date;
  assert( jan01.valid_ymd );
  assert( jan01.valid_hms );
  assert( date->valid_jd );
  jan01.valid_jd = 0;
  jan01.M = 1;
  jan01.D = 1;
  __compute_jd(&jan01);
  return (int)((date->i_jd-jan01.i_jd+43200000)/86400000);
}

/*
** Return the number of days after the most recent Monday.
**
** In other words, return the day of the week according
** to this code:
**
**   0=Monday, 1=Tuesday, 2=Wednesday, ..., 6=Sunday.
*/
static int __days_after_monday(date_time_t *date){
  assert( date->valid_jd );
  return (int)((date->i_jd+43200000)/86400000) % 7;
}

/*
** Return the number of days after the most recent Sunday.
**
** In other words, return the day of the week according
** to this code:
**
**   0=Sunday, 1=Monday, 2=Tuesday, ..., 6=Saturday
*/
static int __days_after_sunday(date_time_t *date){
  assert( date->valid_jd );
  return (int)((date->i_jd+129600000)/86400000) % 7;
}

/*
**    strftime( FORMAT, TIMESTRING, MOD, MOD, ...)
**
** Return a string described by FORMAT.  Conversions as follows:
**
**   %d  day of month  01-31
**   %e  day of month  1-31
**   %f  ** fractional seconds  SS.SSS
**   %F  ISO date.  YYYY-MM-DD
**   %G  ISO year corresponding to %V 0000-9999.
**   %g  2-digit ISO year corresponding to %V 00-99
**   %H  hour 00-24
**   %k  hour  0-24  (leading zero converted to space)
**   %I  hour 01-12
**   %j  day of year 001-366
**   %J  ** julian day number
**   %l  hour  1-12  (leading zero converted to space)
**   %m  month 01-12
**   %M  minute 00-59
**   %p  "AM" or "PM"
**   %P  "am" or "pm"
**   %R  time as HH:MM
**   %s  seconds since 1970-01-01
**   %S  seconds 00-59
**   %T  time as HH:MM:SS
**   %u  day of week 1-7  Monday==1, Sunday==7
**   %w  day of week 0-6  Sunday==0, Monday==1
**   %U  week of year 00-53  (First Sunday is start of week 01)
**   %V  week of year 01-53  (First week containing Thursday is week 01)
**   %W  week of year 00-53  (First Monday is start of week 01)
**   %Y  year 0000-9999
**   %%  %
*/
static void __strftime_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  size_t i,j;
  DBSQL *db;
  const char *fmt;
  dbsql_str_t s_res;


  if( argc==0 ) return;
  fmt = (const char*)dbsql_value_text(argv[0]);
  if( fmt==0 || __is_date(context, argc-1, argv+1, &x) ) return;
  db = dbsql_context_db_handle(context);
  __str_accum_init(&s_res, 0, 0, 0, db->limits[DBSQL_LIMIT_LENGTH]);

  __compute_jd(&x);
  __compute_ymd_hms(&x);
  for(i=j=0; fmt[i]; i++){
    char cf;
    if( fmt[i]!='%' ) continue;
    if( j<i ) dbsql_str_append(&s_res, fmt+j, (int)(i-j));
    i++;
    j = i + 1;
    cf = fmt[i];
    switch( cf ){
      case 'd':  /* Fall thru */
      case 'e': {
        dbsql_str_appendf(&s_res, cf=='d' ? "%02d" : "%2d", x.D);
        break;
      }
      case 'f': {  /* Fractional seconds.  (Non-standard) */
        double s = x.s;
        if( NEVER(s>59.999) ) s = 59.999;
        dbsql_str_appendf(&s_res, "%06.3f", s);
        break;
      }
      case 'F': {
        dbsql_str_appendf(&s_res, "%04d-%02d-%02d", x.Y, x.M, x.D);
        break;
      }
      case 'G': /* Fall thru */
      case 'g': {
        date_time_t y = x;
        assert( y.valid_jd );
        /* Move y so that it is the Thursday in the same week as x */
        y.i_jd += (3 - __days_after_monday(&x))*86400000;
        y.valid_ymd = 0;
        __compute_ymd(&y);
        if( cf=='g' ){
          dbsql_str_appendf(&s_res, "%02d", y.Y%100);
        }else{
          dbsql_str_appendf(&s_res, "%04d", y.Y);
        }
        break;
      }
      case 'H':
      case 'k': {
        dbsql_str_appendf(&s_res, cf=='H' ? "%02d" : "%2d", x.h);
        break;
      }
      case 'I': /* Fall thru */
      case 'l': {
        int h = x.h;
        if( h>12 ) h -= 12;
        if( h==0 ) h = 12;
        dbsql_str_appendf(&s_res, cf=='I' ? "%02d" : "%2d", h);
        break;
      }
      case 'j': {  /* Day of year.  Jan01==1, Jan02==2, and so forth */
        dbsql_str_appendf(&s_res,"%03d",__days_after_jan01(&x)+1);
        break;
      }
      case 'J': {  /* Julian day number.  (Non-standard) */
        dbsql_str_appendf(&s_res,"%.16g",x.i_jd/86400000.0);
        break;
      }
      case 'm': {
        dbsql_str_appendf(&s_res,"%02d",x.M);
        break;
      }
      case 'M': {
        dbsql_str_appendf(&s_res,"%02d",x.m);
        break;
      }
      case 'p': /* Fall thru */
      case 'P': {
        if( x.h>=12 ){
          dbsql_str_append(&s_res, cf=='p' ? "PM" : "pm", 2);
        }else{
          dbsql_str_append(&s_res, cf=='p' ? "AM" : "am", 2);
        }
        break;
      }
      case 'R': {
        dbsql_str_appendf(&s_res, "%02d:%02d", x.h, x.m);
        break;
      }
      case 's': {
        if( x.use_subsec ){
          dbsql_str_appendf(&s_res,"%.3f",
                (x.i_jd - 21086676*(int64_t)10000000)/1000.0);
        }else{
          int64_t i_s = (int64_t)(x.i_jd/1000 - 21086676*(int64_t)10000);
          dbsql_str_appendf(&s_res,"%lld",i_s);
        }
        break;
      }
      case 'S': {
        dbsql_str_appendf(&s_res,"%02d",(int)x.s);
        break;
      }
      case 'T': {
        dbsql_str_appendf(&s_res,"%02d:%02d:%02d", x.h, x.m, (int)x.s);
        break;
      }
      case 'u':    /* Day of week.  1 to 7.  Monday==1, Sunday==7 */
      case 'w': {  /* Day of week.  0 to 6.  Sunday==0, Monday==1 */
        char c = (char)__days_after_sunday(&x) + '0';
        if( c=='0' && cf=='u' ) c = '7';
        dbsql_str_appendchar(&s_res, 1, c);
        break;
      }
      case 'U': {  /* Week num. 00-53. First Sun of the year is week 01 */
        dbsql_str_appendf(&s_res,"%02d",
              (__days_after_jan01(&x)-__days_after_sunday(&x)+7)/7);
        break;
      }
      case 'V': {  /* Week num. 01-53. First week with a Thur is week 01 */
        date_time_t y = x;
        /* Adjust y so that is the Thursday in the same week as x */
        assert( y.valid_jd );
        y.i_jd += (3 - __days_after_monday(&x))*86400000;
        y.valid_ymd = 0;
        __compute_ymd(&y);
        dbsql_str_appendf(&s_res,"%02d", __days_after_jan01(&y)/7+1);
        break;
      }
      case 'W': {  /* Week num. 00-53. First Mon of the year is week 01 */
        dbsql_str_appendf(&s_res,"%02d",
           (__days_after_jan01(&x)-__days_after_monday(&x)+7)/7);
        break;
      }
      case 'Y': {
        dbsql_str_appendf(&s_res,"%04d",x.Y);
        break;
      }
      case '%': {
        dbsql_str_appendchar(&s_res, 1, '%');
        break;
      }
      default: {
        dbsql_str_reset(&s_res);
        return;
      }
    }
  }
  if( j<i ) dbsql_str_append(&s_res, fmt+j, (int)(i-j));
  __result_str_accum(context, &s_res);
}

/*
** current_time()
**
** This function returns the same value as time('now').
*/
static void __ctime_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  __time_func(context, 0, 0);
}

/*
** current_date()
**
** This function returns the same value as date('now').
*/
static void __cdate_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  __date_func(context, 0, 0);
}

/*
** timediff(DATE1, DATE2)
**
** Return the amount of time that must be added to DATE2 in order to
** convert it into DATE2.  The time difference format is:
**
**     +YYYY-MM-DD HH:MM:SS.SSS
**
** The initial "+" becomes "-" if DATE1 occurs before DATE2.  For
** date/time values A and B, the following invariant should hold:
**
**     datetime(A) == (datetime(B, timediff(A,B))
**
** Both DATE arguments must be either a julian day number, or an
** ISO-8601 string.  The unix timestamps are not supported by this
** routine.
*/
static void __timediff_func(
  dbsql_func_t *context,
  int __not_used1,
  dbsql_value_t **argv
){
  char sign;
  int Y, M;
  date_time_t d1, d2;
  dbsql_str_t s_res;
  UNUSED_PARAMETER(__not_used1);
  if( __is_date(context, 1, &argv[0], &d1) ) return;
  if( __is_date(context, 1, &argv[1], &d2) ) return;
  __compute_ymd_hms(&d1);
  __compute_ymd_hms(&d2);
  if( d1.i_jd>=d2.i_jd ){
    sign = '+';
    Y = d1.Y - d2.Y;
    if( Y ){
      d2.Y = d1.Y;
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    M = d1.M - d2.M;
    if( M<0 ){
      Y--;
      M += 12;
    }
    if( M!=0 ){
      d2.M = d1.M;
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    while( d1.i_jd<d2.i_jd ){
      M--;
      if( M<0 ){
        M = 11;
        Y--;
      }
      d2.M--;
      if( d2.M<1 ){
        d2.M = 12;
        d2.Y--;
      }
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    d1.i_jd -= d2.i_jd;
    d1.i_jd += (u_int64_t)1486995408 * (u_int64_t)100000;
  }else /* d1<d2 */{
    sign = '-';
    Y = d2.Y - d1.Y;
    if( Y ){
      d2.Y = d1.Y;
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    M = d2.M - d1.M;
    if( M<0 ){
      Y--;
      M += 12;
    }
    if( M!=0 ){
      d2.M = d1.M;
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    while( d1.i_jd>d2.i_jd ){
      M--;
      if( M<0 ){
        M = 11;
        Y--;
      }
      d2.M++;
      if( d2.M>12 ){
        d2.M = 1;
        d2.Y++;
      }
      d2.valid_jd = 0;
      __compute_jd(&d2);
    }
    d1.i_jd = d2.i_jd - d1.i_jd;
    d1.i_jd += (u_int64_t)1486995408 * (u_int64_t)100000;
  }
  __clear_ymd_hms_tz(&d1);
  __compute_ymd_hms(&d1);
  __str_accum_init(&s_res, 0, 0, 0, 100);
  dbsql_str_appendf(&s_res, "%c%04d-%02d-%02d %02d:%02d:%06.3f",
       sign, Y, M, d1.D-1, d1.h, d1.m, d1.s);
  __result_str_accum(context, &s_res);
}


/*
** current_timestamp()
**
** This function returns the same value as datetime('now').
*/
static void __ctimestamp_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  __datetime_func(context, 0, 0);
}
#endif /* !defined(DBSQL_OMIT_DATETIME_FUNCS) */

#ifdef DBSQL_OMIT_DATETIME_FUNCS
/*
** If the library is compiled to omit the full-scale date and time
** handling (to get a smaller binary), the following minimal version
** of the functions current_time(), current_date() and current_timestamp()
** are included instead. This is to support column declarations that
** include "DEFAULT CURRENT_TIME" etc.
**
** This function uses the C-library functions time(), gmtime()
** and strftime(). The format string to pass to strftime() is supplied
** as the user-data for the function.
*/
static void __current_time_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  time_t t;
  char *format = (char *)dbsql_user_data(context);
  int64_t i_t;
  struct tm *p_tm;
  struct tm s_now;
  char buf[20];

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);

  i_t = __stmt_current_time(context);
  if( i_t<=0 ) return;
  t = i_t/1000 - 10000*(int64_t)21086676;
#if HAVE_GMTIME_R
  p_tm = gmtime_r(&t, &s_now);
#else
  dbsql_mutex_enter(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN));
  p_tm = gmtime(&t);
  if( p_tm ) memcpy(&s_now, p_tm, sizeof(s_now));
  dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN));
#endif
  if( p_tm ){
    strftime(buf, 20, format, &s_now);
    dbsql_result_text(context, buf, -1, DBSQL_TRANSIENT);
  }
}
#endif

#if !defined(DBSQL_OMIT_DATETIME_FUNCS) && defined(DBSQL_DEBUG)
/*
**   datedebug(...)
**
** This routine returns JSON that describes the internal date_time_t object.
** Used for debugging and testing only.  Subject to change.
*/
static void __datedebug_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  date_time_t x;
  if( __is_date(context, argc, argv, &x)==0 ){
    char *json;
    json = dbsql_mprintf(
      "{iJD:%lld,Y:%d,M:%d,D:%d,h:%d,m:%d,tz:%d,"
      "s:%.3f,validJD:%d,validYMD:%d,validHMS:%d,"
      "nFloor:%d,rawS:%d,isError:%d,useSubsec:%d,"
      "isUtc:%d,isLocal:%d}",
      x.i_jd, x.Y, x.M, x.D, x.h, x.m, x.tz,
      x.s, x.valid_jd, x.valid_ymd, x.valid_hms,
      x.n_floor, x.raw_s, x.isError, x.use_subsec,
      x.is_utc, x.isLocal);
    dbsql_result_text(context, json, -1, dbsql_free);
  }
}
#endif /* !DBSQL_OMIT_DATETIME_FUNCS && DBSQL_DEBUG */


/*
** This function registered all of the above C functions as SQL
** functions.  This should be the only routine in this file with
** external linkage.
*/
void __register_date_time_functions(void){
  static func_def_t date_time_funcs[] = {
#ifndef DBSQL_OMIT_DATETIME_FUNCS
    PURE_DATE(julianday,        -1, 0, 0, __julianday_func ),
    PURE_DATE(unixepoch,        -1, 0, 0, __unixepoch_func ),
    PURE_DATE(date,             -1, 0, 0, __date_func      ),
    PURE_DATE(time,             -1, 0, 0, __time_func      ),
    PURE_DATE(datetime,         -1, 0, 0, __datetime_func  ),
    PURE_DATE(strftime,         -1, 0, 0, __strftime_func  ),
    PURE_DATE(timediff,          2, 0, 0, __timediff_func  ),
#ifdef DBSQL_DEBUG
    PURE_DATE(datedebug,        -1, 0, 0, __datedebug_func ),
#endif
    DFUNCTION(current_time,      0, 0, 0, __ctime_func     ),
    DFUNCTION(current_timestamp, 0, 0, 0, __ctimestamp_func),
    DFUNCTION(current_date,      0, 0, 0, __cdate_func     ),
#else
    STR_FUNCTION(current_time,      0, "%H:%M:%S",          0, __current_time_func),
    STR_FUNCTION(current_date,      0, "%Y-%m-%d",          0, __current_time_func),
    STR_FUNCTION(current_timestamp, 0, "%Y-%m-%d %H:%M:%S", 0, __current_time_func),
#endif
  };
  __insert_builtin_funcs(date_time_funcs, ARRAY_SIZE(date_time_funcs));
}
