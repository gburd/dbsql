# $Id: options.ac 7 2007-02-03 13:34:17Z gburd $

# Process user-specified options.
AC_DEFUN(AM_OPTIONS_SET, [

AC_MSG_CHECKING(if --enable-utf8-encoding option specified)
AC_ARG_ENABLE(utf8-encoding,
	AC_HELP_STRING([--enable-utf8-encoding],
	    [Encode strings as UTF-8 or ISO-8859.]),, enableval="yes")
db_cv_utf8_strings="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(yes);;
yes) AC_MSG_RESULT(no);;
esac
if test "$cv_utf8_strings" = "yes"; then
  ENCODING=UTF8
  AC_DEFINE(DBSQL_UTF8_ENCODING)
else
  ENCODING=ISO8859
  AC_DEFINE(DBSQL_ISO8859_ENCODING)
fi
AH_TEMPLATE(DBSQL_ISO8859_ENCODING,
    [Define to use ISO8859 string encoding.])
AH_TEMPLATE(DBSQL_UTF8_ENCODING,
    [Define use UTF8 string encoding.])


AC_MSG_CHECKING(if --enable-incore-databases option specified)
AC_ARG_ENABLE(incore-databases,
	AC_HELP_STRING([--enable-incore-databases],
	    [Enable incore databases for ATTACH and OPEN.]),, enableval="yes")
db_cv_incore_databases="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(yes);;
yes) AC_MSG_RESULT(no);;
esac
if test "$db_cv_incore_databases" = "no"; then
  INMEMORYDB=0
else
  INMEMORYDB=1
fi
AC_DEFINE(INMEMORYDB)
AH_TEMPLATE(INMEMORYDB,
    [Define to 1 to enable incore databases for ATTACH and OPEN.])

AC_MSG_CHECKING(if --enable-authentication option specified)
AC_ARG_ENABLE(authentication,
	AC_HELP_STRING([--enable-authentication],
	    [Include authorization validation code.]),, enableval="yes")
db_cv_auth="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(no);;
yes) AC_MSG_RESULT(yes);;
esac
if test "$db_cv_auth" = "no"; then
  AC_DEFINE(DBSQL_OMIT_AUTHORIZATION)
fi
AH_TEMPLATE(DBSQL_OMIT_AUTHORIZATION,
    [Define to 1 to omit authorization code from the build.])


AC_MSG_CHECKING(if --enable-vacuum option specified)
AC_ARG_ENABLE(vacuum,
	AC_HELP_STRING([--enable-vacuum],
	    [Include the VACUUM command.]),, enableval="yes")
db_cv_auth="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(no);;
yes) AC_MSG_RESULT(yes);;
esac
if test "$db_cv_auth" = "no"; then
  AC_DEFINE(DBSQL_OMIT_VACUUM)
fi
AH_TEMPLATE(DBSQL_OMIT_VACUUM,
    [Define to 1 to omit the code for the VACCUM command from the build.])

AC_MSG_CHECKING(if --enable-datetime option specified)
AC_ARG_ENABLE(enable-datetime,
	AC_HELP_STRING([--enable-datetime],
	    [Include datetime functions.]),, enableval="yes")
db_cv_datetime="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(no);;
yes) AC_MSG_RESULT(yes);;
esac
if test "$db_cv_datetime" = "no"; then
  AC_DEFINE(DBSQL_OMIT_DATETIME_FUNCS)
      
fi
AH_TEMPLATE(DBSQL_OMIT_DATETIME_FUNCS,
    [Define to 1 to omit support for datetime functions from the build.])


AC_MSG_CHECKING([if --with-berkeleydb=DIR option specified])
AC_ARG_WITH(berkeleydb,
	[AC_HELP_STRING([--with-berkeleydb=DIR],
		[Path of Berkeley DB. [DIR="/usr/local/BerkeleyDB.4.5"]])],
	[with_berkeleydb="$withval"], [with_berkeleydb="no"])
AC_MSG_RESULT($with_berkeleydb)
# If --with-berkeleydb isn't specified, assume it's here | wc -l` -gt 0
if test "$with_berkeleydb" = "no"; then
	with_berkeleydb="/usr/local/BerkeleyDB.4.5"
fi
DB_PATH="$with_berkeleydb"
if test `ls "$with_berkeleydb"/lib/libdb-*.la 2>/dev/null | wc -l` -gt 0 ; then
	AC_MSG_CHECKING([for Berkeley DB version from install tree])
	db_version=`ls "$with_berkeleydb"/lib/libdb-*.la | sed 's/.*db-\(.*\).la/\1/'`
	AC_MSG_RESULT([$db_version])
	echo "$CPPFLAGS" | grep "$with_berkeleydb/include" >/dev/null 2>&1 || CPPFLAGS="$CPPFLAGS -I$with_berkeleydb/include"
	if test `ls "$with_berkeleydb"/lib/libdb-$db_version.* 2>/dev/null | wc -l` -gt 0 ; then
		LIBSO_LIBS="$LIBS -L$with_berkeleydb/lib -ldb-$db_version"
	else
		LIBS="$LIBS -l$with_berkeleydb/lib/libdb-$db_version.a"
	fi
elif test `ls /usr/local/lib/db?? 2>/dev/null | wc -l` -gt 0 ; then
	AC_MSG_CHECKING([if Berkeley DB was installed using BSD ports])
	db_num=`ls /usr/local/lib | grep db | grep -v lib | sed -e 's/db//' | sort -n | head -1`
	db_version=`echo $db_num | sed 's/\(.\)\(.\)/\1.\2/'`
	AC_MSG_RESULT([yes, $db_version])
	echo "$CPPFLAGS" | grep /usr/local/include/db$db_num >/dev/null 2>&1 || CPPFLAGS="$CPPFLAGS -I/usr/local/include/db$db_num"
	if test `ls /usr/local/lib/libdb-$db_version.* 2>/dev/null | wc -l` -gt 0 ; then
		LIBSO_LIBS="$LIBS -L/usr/local/lib -ldb-$db_version"
	else
		LIBS="$LIBS -l/usr/local/lib/libdb-$db_version.a"
	fi
else
	AC_MSG_ERROR([$with_berkeleydb not a valid Berkeley DB install tree.])
fi

AC_MSG_CHECKING([if --with-db-uniquename=NAME option specified])
AC_ARG_WITH(db-uniquename,
	[AC_HELP_STRING([--with-db-uniquename=NAME],
			[Unique name used when building DB library.])],
	[with_db_uniquename="$withval"], [with_db_uniquename="no"])
if test "$with_db_uniquename" = "no"; then
	db_cv_uniquename="no"
	DB_UNIQUE_NAME=""
	AC_MSG_RESULT($with_db_uniquename)
else
	db_cv_uniquename="yes"
	if test "$with_db_uniquename" = "yes"; then
		DB_UNIQUE_NAME="__EDIT_DB_VERSION_UNIQUE_NAME__"
	else
		DB_UNIQUE_NAME="$with_db_uniquename"
	fi
	AC_MSG_RESULT($DB_UNIQUE_NAME)
fi

AC_MSG_CHECKING(if --enable-incore-temp-databases option specified)
AC_ARG_ENABLE(incore-temp-databases,
	AC_HELP_STRING([--enable-incore-temp-databases],
	    [Enable incore databases for temporary tables.]),, enableval="no")
db_cv_incore_temp_databases="$enableval"
case "$db_cv_incore_temp_databases" in
  never) 
    TEMP_STORE=0
    AC_MSG_RESULT([never])
  ;;
  no)
    INMEMORYDB=1
    TEMP_STORE=1
    AC_MSG_RESULT([no])
  ;;
  always) 
    INMEMORYDB=1
    TEMP_STORE=3
    AC_MSG_RESULT([always])
  ;;
  *) 
    INMEMORYDB=1
    TEMP_STORE=2
    AC_MSG_RESULT([yes])
  ;;
esac
AC_DEFINE(INMEMORYDB)
AH_TEMPLATE(INMEMORYDB,
    [Define to 1 to enable memory resident databases.])
AC_DEFINE(TEMP_STORE)
AH_TEMPLATE(TEMP_STORE,
    [Determines where TEMP databases can be stored, see table in source code.])

AC_MSG_CHECKING(if --disable-statistics option specified)
AC_ARG_ENABLE(statistics,
	AC_HELP_STRING([--disable-statistics],
	    [Do not build statistics support.]),, enableval="yes")
db_cv_build_statistics="$enableval"
case "$enableval" in
 no) AC_MSG_RESULT(yes);;
yes) AC_MSG_RESULT(no);;
esac

AC_MSG_CHECKING(if --enable-sqlite-compat option specified)
AC_ARG_ENABLE(sqlite-compat,
	[AC_HELP_STRING([--enable-sqlite-compat],
			[Build SQLite compatibility API.])],
	[db_cv_sqlite_compat="$enable_sqlite_compat"],
	[db_cv_sqlite_compat="no"])
AC_MSG_RESULT($db_cv_sqlite_compat)

AC_MSG_CHECKING(if --enable-soundex-sqlfn option specified)
AC_ARG_ENABLE(soundex-sqlfn,
	[AC_HELP_STRING([--enable-soundex-sqlfn],
			[Include soundex() sql function support.])],
	[db_cv_sqlite_compat="$enable_soundex_sqlfn"],
	[db_cv_sqlite_compat="no"])
AC_MSG_RESULT($db_cv_soundex_sqlfn)

AC_MSG_CHECKING(if --enable-posixmutexes option specified)
AC_ARG_ENABLE(posixmutexes,
	[AC_HELP_STRING([--enable-posixmutexes],
			[Force use of POSIX standard mutexes.])],
	[db_cv_posixmutexes="$enable_posixmutexes"], [db_cv_posixmutexes="no"])
AC_MSG_RESULT($db_cv_posixmutexes)

AC_MSG_CHECKING(if --enable-debug option specified)
AC_ARG_ENABLE(debug,
	[AC_HELP_STRING([--enable-debug],
			[Build a debugging version.])],
	[db_cv_debug="$enable_debug"], [db_cv_debug="no"])
AC_MSG_RESULT($db_cv_debug)

AC_MSG_CHECKING(if --enable-diagnostic option specified)
AC_ARG_ENABLE(diagnostic,
	[AC_HELP_STRING([--enable-diagnostic],
			[Build a version with run-time diagnostics.])],
	[db_cv_diagnostic="$enable_diagnostic"], [db_cv_diagnostic="no"])
AC_MSG_RESULT($db_cv_diagnostic)

AC_MSG_CHECKING(if --enable-tcl option specified)
AC_ARG_ENABLE(tcl,
	[AC_HELP_STRING([--enable-tcl],
			[Build Tcl API.])],
	[db_cv_tcl="$enable_tcl"], [db_cv_tcl="no"])
AC_MSG_RESULT($db_cv_tcl)

AC_MSG_CHECKING([if --with-tcl=DIR option specified])
AC_ARG_WITH(tcl,
	[AC_HELP_STRING([--with-tcl=DIR],
			[Directory location of tclConfig.sh.])],
	[with_tclconfig="$withval"], [with_tclconfig="no"])
AC_MSG_RESULT($with_tclconfig)

AC_MSG_CHECKING(if --enable-test option specified)
AC_ARG_ENABLE(test,
	[AC_HELP_STRING([--enable-test],
			[Configure to run the test suite.])],
	[db_cv_test="$enable_test"], [db_cv_test="no"])
AC_MSG_RESULT($db_cv_test)

# Test requires Tcl
if test "$db_cv_test" = "yes"; then
	if test "$db_cv_tcl" = "no"; then
		AC_MSG_ERROR([--enable-test requires --enable-tcl])
	fi
fi])
