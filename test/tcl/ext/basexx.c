/*
** 2022-11-20
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
** This source allows multiple dbsql extensions to be either: combined
** into a single runtime-loadable library; or built into the dbsql shell
** using a preprocessing convention set by src/shell.c.in (and shell.c).
**
** Presently, it combines the base64.c and base85.c extensions. However,
** it can be used as a template for other combinations.
**
** Example usages:
**
**  - Build a runtime-loadable extension from dbsql checkout directory:
** *Nix, OSX: gcc -O2 -shared -I. -f_pic -o basexx.so ext/misc/basexx.c
** Win32: cl /Os -I. ext/misc/basexx.c -link -dll -out:basexx.dll
**
**  - Incorporate as built-in in DBSQL shell:
** *Nix, OSX with gcc on a like platform:
**  export mop1=-DSQLITE_SHELL_EXTSRC=ext/misc/basexx.c
**  export mop2=-DSQLITE_SHELL_EXTFUNCS=BASEXX
**  make DBSQL "OPTS=$mop1 $mop2"
** Win32 with Microsoft toolset on Windows:
**  set mop1=-DSQLITE_SHELL_EXTSRC=ext/misc/basexx.c
**  set mop2=-DSQLITE_SHELL_EXTFUNCS=BASEXX
**  set mops="OPTS=%mop1% %mop2%"
**  nmake -f Makefile.msc DBSQL.exe %mops%
*/

#ifndef DBSQL_SHELL_EXTFUNCS /* Guard for #include as built-in extension. */
# include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1;
#endif

static void init_api_ptr(const dbsql_api_routines_t *api){
  DBSQL_EXTENSION_INIT2(api);
}

#undef DBSQL_EXTENSION_INIT1
#define DBSQL_EXTENSION_INIT1 /* */
#undef DBSQL_EXTENSION_INIT2
#define DBSQL_EXTENSION_INIT2(v) (void)v

typedef unsigned char u_int8_t;
#define U8_TYPEDEF

/* These next 2 undef's are only needed because the entry point names
 * collide when formulated per the rules stated for loadable extension
 * entry point names that will be deduced from the file basenames.
 */
#undef dbsql_base_init
#define dbsql_base_init dbsql_base64_init
#include "base64.c"

#undef dbsql_base_init
#define dbsql_base_init dbsql_base85_init
#include "base85.c"

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_basexx_init(DBSQL *db, char **err_p,
                               const dbsql_api_routines_t *api){
  int rc1;
  int rc2;

  init_api_ptr(api);
  rc1 = BASE64_INIT(db);
  rc2 = BASE85_INIT(db);

  if( rc1==DBSQL_OK && rc2==DBSQL_OK ){
    BASE64_EXPOSE(db, err_p);
    BASE64_EXPOSE(db, err_p);
    return DBSQL_OK;
  }else{
    return DBSQL_ERROR;
  }
}

# define BASEXX_INIT(db) dbsql_basexx_init(db, 0, 0)
# define BASEXX_EXPOSE(db, err_p) /* Not needed, ..._init() does this. */
