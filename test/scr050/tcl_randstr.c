/*
 * Implemntation of additional sql functions useful from the TCL
 * interface.  Most of these are for use during testing.
 */
#include "dbsql_config.h"
#include "dbsql_int.h"
#include "inc/os_ext.h"
#include "tcl.h"

#include <stdlib.h>
#include <string.h>

/*
** This function generates a string of random characters.  Used for
** generating test data.
*/
void
__tcl_sql_func_randstr(dbsql_func_t *context, int argc, const char **argv){
  static const char zSrc[] = 
     "abcdefghijklmnopqrstuvwxyz"
     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
     "0123456789"
     ".-!,:*^+=_|?/<> ";
  int iMin, iMax, n, r, i;
  char zBuf[1000];
  if( argc>=1 ){
    iMin = atoi(argv[0]);
    if( iMin<0 ) iMin = 0;
    if( iMin>=sizeof(zBuf) ) iMin = sizeof(zBuf)-1;
  }else{
    iMin = 1;
  }
  if( argc>=2 ){
    iMax = atoi(argv[1]);
    if( iMax<iMin ) iMax = iMin;
    if( iMax>=sizeof(zBuf) ) iMax = sizeof(zBuf)-1;
  }else{
    iMax = 50;
  }
  n = iMin;
  if( iMax>iMin ){
    r = __random_int() & 0x7fffffff;
    n += r%(iMax + 1 - iMin);
  }
  DBSQL_ASSERT( n<sizeof(zBuf) );
  r = 0;
  for(i=0; i<n; i++){
    r = (r + __random_byte())% (sizeof(zSrc)-1);
    zBuf[i] = zSrc[r];
  }
  zBuf[n] = 0;
  dbsql_set_result_string(context, zBuf, n);
}
