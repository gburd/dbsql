# DBSQL - A SQL database engine.
#
# Copyright (C) 2007-2026  The DBSQL Group, Inc.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# Process with autoconf (dist/s_config) to produce dist/configure.
# Build in build_unix/:  cd build_unix && ../dist/configure && gmake

AC_INIT([dbsql],[__EDIT_DBSQL_VERSION__],[greg@burd.me],[dbsql])
AC_CONFIG_SRCDIR([../src/sql_parser.y])
AC_CONFIG_HEADERS([dbsql_config.h:config.hin])
AC_CONFIG_AUX_DIR([.])
AC_CANONICAL_HOST
AC_LANG([C])

AC_MSG_CHECKING([that we are not building in the top-level or dist directory])
if test -d aclocal -o -f ../dist/RELEASE -a ! -f ../dist/configure.ac; then
	AC_MSG_ERROR([build in build_unix/ (or any other directory)])
fi
AC_MSG_RESULT([ok])

DBSQL_VERSION_MAJOR="__EDIT_DBSQL_VERSION_MAJOR__"
DBSQL_VERSION_MINOR="__EDIT_DBSQL_VERSION_MINOR__"
DBSQL_VERSION_PATCH="__EDIT_DBSQL_VERSION_PATCH__"
DBSQL_VERSION="__EDIT_DBSQL_VERSION__"
DBSQL_VERSION_NUMBER="__EDIT_DBSQL_VERSION_NUMBER__"
DBSQL_VERSION_STRING="__EDIT_DBSQL_VERSION_STRING__"
DBSQL_SQLITE_VERSION="__EDIT_DBSQL_SQLITE_VERSION__"
DBSQL_LIBDB_VERSION="__EDIT_DBSQL_LIBDB_VERSION__"
AC_SUBST(DBSQL_VERSION_MAJOR)
AC_SUBST(DBSQL_VERSION_MINOR)
AC_SUBST(DBSQL_VERSION_PATCH)
AC_SUBST(DBSQL_VERSION)
AC_SUBST(DBSQL_VERSION_NUMBER)
AC_SUBST(DBSQL_VERSION_STRING)
AC_SUBST(DBSQL_SQLITE_VERSION)
AC_SUBST(DBSQL_LIBDB_VERSION)

AC_PROG_CC
AC_PROG_INSTALL
LT_INIT([shared static])
AC_PATH_PROG([TCLSH], [tclsh8.6 tclsh86 tclsh9.0 tclsh], [no])

# --------------------------------------------------------------------
# Options.
# --------------------------------------------------------------------
AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug], [build with -g -O0])],
    [], [enable_debug=no])
AC_ARG_ENABLE([diagnostic],
    [AS_HELP_STRING([--enable-diagnostic],
	[run-time assertions (DBSQL_DEBUG)])],
    [], [enable_diagnostic=no])
AC_ARG_ENABLE([asan],
    [AS_HELP_STRING([--enable-asan], [AddressSanitizer + UBSan])],
    [], [enable_asan=no])
AC_ARG_ENABLE([test],
    [AS_HELP_STRING([--enable-test], [build the Tcl testfixture])],
    [], [enable_test=no])
AC_ARG_ENABLE([tcl],
    [AS_HELP_STRING([--enable-tcl], [build the Tcl package "dbsql"])],
    [], [enable_tcl=no])
AC_ARG_ENABLE([fts5],
    [AS_HELP_STRING([--disable-fts5], [omit FTS5 full-text search])],
    [], [enable_fts5=yes])
AC_ARG_ENABLE([icu],
    [AS_HELP_STRING([--enable-icu], [ICU collations and case folding (auto)])],
    [], [enable_icu=auto])
AC_ARG_ENABLE([math],
    [AS_HELP_STRING([--disable-math], [omit SQL math functions])],
    [], [enable_math=yes])
AC_ARG_WITH([berkeleydb],
    [AS_HELP_STRING([--with-berkeleydb=DIR],
	[libdb install prefix or build_unix directory
	 (default: ../deps/libdb/build_unix)])],
    [], [with_berkeleydb=default])
AC_ARG_WITH([tcl],
    [AS_HELP_STRING([--with-tcl=DIR], [directory containing tclConfig.sh])],
    [], [with_tcl=no])
AC_ARG_WITH([readline],
    [AS_HELP_STRING([--with-readline],
	[line editing in the shell: editline (default if found), readline, no])],
    [], [with_readline=auto])
AC_ARG_WITH([storage],
    [AS_HELP_STRING([--with-storage=libdb|native],
	[storage engine; "native" is SQLite's own b-tree, for transform
	 validation only, never shipped (default: libdb)])],
    [], [with_storage=libdb])

# --------------------------------------------------------------------
# Compiler flags and SQL-layer defines.
# --------------------------------------------------------------------
DBSQL_CFLAGS=""
if test "$enable_debug" = yes; then
	CFLAGS="-g -O0"
else
	test -z "$ac_cv_env_CFLAGS_set" && CFLAGS="-g -O2"
fi
DBSQL_DEFS="-DDBSQL_CORE -DDBSQL_THREADSAFE=1 -DDBSQL_ENABLE_PERCENTILE -DDBSQL_ENABLE_CARRAY -D_HAVE_DBSQL_CONFIG_H"
test "$enable_math" = yes && DBSQL_DEFS="$DBSQL_DEFS -DDBSQL_ENABLE_MATH_FUNCTIONS"
test "$enable_fts5" = yes && DBSQL_DEFS="$DBSQL_DEFS -DDBSQL_ENABLE_FTS5"
test "$enable_diagnostic" = yes && DBSQL_DEFS="$DBSQL_DEFS -DDBSQL_DEBUG=1"
if test "$enable_asan" = yes; then
	DBSQL_CFLAGS="$DBSQL_CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
	LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
fi
if test "$with_storage" = libdb; then
	DBSQL_DEFS="$DBSQL_DEFS -DDBSQL_OMIT_SHARED_CACHE -DDBSQL_OMIT_WAL -DDBSQL_OMIT_DESERIALIZE"
fi
AC_SUBST(DBSQL_CFLAGS)
AC_SUBST(DBSQL_DEFS)
AC_SUBST(with_storage)

# --------------------------------------------------------------------
# libdb.
# --------------------------------------------------------------------
if test "$with_storage" = libdb; then
	if test "$with_berkeleydb" = default; then
		with_berkeleydb=`cd "$srcdir/.." && pwd`/deps/libdb/build_unix
	fi
	AC_MSG_CHECKING([for libdb in $with_berkeleydb])
	if test -f "$with_berkeleydb/db.h" && \
	    ls "$with_berkeleydb"/libdb-*.la >/dev/null 2>&1; then
		DB_CPPFLAGS="-I$with_berkeleydb"
		DB_LIBDIR="$with_berkeleydb/.libs"
	elif test -f "$with_berkeleydb/include/db.h"; then
		DB_CPPFLAGS="-I$with_berkeleydb/include"
		DB_LIBDIR="$with_berkeleydb/lib"
	else
		AC_MSG_RESULT([no])
		AC_MSG_ERROR([no db.h under $with_berkeleydb; build deps/libdb
	(deps/build_libdb.sh) or pass --with-berkeleydb=PREFIX])
	fi
	db_lib=`ls "$DB_LIBDIR" | sed -n 's/^lib\(db-[[0-9]][[0-9.]]*\)\.so$/\1/p' | sort | tail -1`
	test -z "$db_lib" && db_lib=`ls "$DB_LIBDIR" | sed -n 's/^lib\(db-[[0-9]][[0-9.]]*\)\.a$/\1/p' | sort | tail -1`
	if test -z "$db_lib"; then
		AC_MSG_RESULT([no])
		AC_MSG_ERROR([no libdb-*.so or .a in $DB_LIBDIR])
	fi
	AC_MSG_RESULT([$DB_CPPFLAGS -l$db_lib])
	DB_LIBS="-L$DB_LIBDIR -l$db_lib -Wl,-rpath,$DB_LIBDIR"
	save_CPPFLAGS="$CPPFLAGS"
	CPPFLAGS="$CPPFLAGS $DB_CPPFLAGS"
	# libdb's numeric version is its format/ABI level and stays put across
	# releases; DB_CALVER ("2026.10.4") names the release.
	AC_PROG_CPP
	AC_MSG_CHECKING([libdb release >= $DBSQL_LIBDB_VERSION])
	db_calver=`printf '#include <db.h>\nDB_CALVER\n' |
	    $CPP $CPPFLAGS - 2>/dev/null | sed -n 's/^"\([[0-9.]]*\)"$/\1/p'`
	AS_VERSION_COMPARE([$db_calver], [$DBSQL_LIBDB_VERSION],
	    [db_calver_ok=no], [db_calver_ok=yes], [db_calver_ok=yes])
	test -n "$db_calver" || db_calver_ok=no
	AC_MSG_RESULT([$db_calver_ok (${db_calver:-unknown})])
	test "$db_calver_ok" = yes ||
	    AC_MSG_ERROR([dbsql needs libdb v$DBSQL_LIBDB_VERSION or later])
	CPPFLAGS="$save_CPPFLAGS"
fi
AC_SUBST(DB_CPPFLAGS)
AC_SUBST(DB_LIBS)

# --------------------------------------------------------------------
# System features used by the SQL layer and the shell.
# --------------------------------------------------------------------
AC_CHECK_HEADERS([stdint.h inttypes.h malloc.h])
AC_CHECK_TYPES([int8_t, int16_t, uint8_t, uint16_t, uint32_t])
AC_CHECK_FUNCS([usleep nanosleep localtime_r gmtime_r isnan fdatasync \
	pread pwrite strchrnul strerror_r utime readlink lstat fchown \
	fchmod posix_fallocate malloc_usable_size log2 log10])
AC_SEARCH_LIBS([log], [m])
AC_SEARCH_LIBS([pthread_create], [pthread])
AC_SEARCH_LIBS([dlopen], [dl])
AC_CHECK_LIB([z], [deflate],
    [AC_DEFINE([DBSQL_HAVE_ZLIB], [1], [zlib for the shell's zipfile])
     SHELL_LIBS="$SHELL_LIBS -lz"])

# Line editing for the shell.
if test "$with_readline" != no; then
	if test "$with_readline" != readline; then
		save_CPPFLAGS="$CPPFLAGS"
		CPPFLAGS="$CPPFLAGS -I/usr/local/include"
		AC_CHECK_HEADER([editline/readline.h],
		    [AC_CHECK_LIB([edit], [readline],
			[AC_DEFINE([HAVE_EDITLINE], [1], [libedit])
			 SHELL_CPPFLAGS="-I/usr/local/include"
			 SHELL_LIBS="$SHELL_LIBS -L/usr/local/lib -ledit"
			 with_readline=editline], [], [-L/usr/local/lib])])
		CPPFLAGS="$save_CPPFLAGS"
	fi
	if test "$with_readline" = readline -o "$with_readline" = auto; then
		AC_CHECK_HEADER([readline/readline.h],
		    [AC_CHECK_LIB([readline], [readline],
			[AC_DEFINE([HAVE_READLINE], [1], [GNU readline])
			 SHELL_LIBS="$SHELL_LIBS -lreadline"])])
	fi
fi
AC_SUBST(SHELL_CPPFLAGS)
AC_SUBST(SHELL_LIBS)

# ICU.
ICU_CFLAGS=""
ICU_LIBS=""
if test "$enable_icu" != no; then
	AC_PATH_PROG([PKG_CONFIG], [pkg-config pkgconf], [no])
	if test "$PKG_CONFIG" != no && \
	    $PKG_CONFIG --exists icu-uc icu-i18n 2>/dev/null; then
		ICU_CFLAGS=`$PKG_CONFIG --cflags icu-uc icu-i18n`
		ICU_LIBS=`$PKG_CONFIG --libs icu-uc icu-i18n`
		DBSQL_DEFS="$DBSQL_DEFS -DDBSQL_ENABLE_ICU"
		enable_icu=yes
	elif test "$enable_icu" = yes; then
		AC_MSG_ERROR([--enable-icu: ICU not found via pkg-config])
	else
		enable_icu=no
	fi
fi
AC_SUBST(ICU_CFLAGS)
AC_SUBST(ICU_LIBS)
AC_SUBST(enable_icu)
AC_SUBST(enable_fts5)

# Tcl (package and/or testfixture).
TCL_CPPFLAGS=""
TCL_LIBS=""
if test "$enable_tcl" = yes -o "$enable_test" = yes; then
	if test "$with_tcl" = no; then
		for d in /usr/local/lib/tcl8.6 /usr/lib/tcl8.6 /usr/lib64/tcl8.6 \
		    /usr/lib /usr/local/lib; do
			test -f "$d/tclConfig.sh" && { with_tcl=$d; break; }
		done
	fi
	if test ! -f "$with_tcl/tclConfig.sh"; then
		AC_MSG_ERROR([tclConfig.sh not found; use --with-tcl=DIR])
	fi
	. "$with_tcl/tclConfig.sh"
	TCL_CPPFLAGS="$TCL_INCLUDE_SPEC"
	eval "TCL_LIBS=\"$TCL_LIB_SPEC\""
fi
AC_SUBST(TCL_CPPFLAGS)
AC_SUBST(TCL_LIBS)
AC_SUBST(enable_tcl)
AC_SUBST(enable_test)

AC_CONFIG_FILES([Makefile dbsql.pc])
AC_OUTPUT

echo
echo "dbsql $DBSQL_VERSION configured:"
echo "  storage:   $with_storage ${DB_CPPFLAGS:+($DB_CPPFLAGS)}"
echo "  fts5:      $enable_fts5   icu: $enable_icu   math: $enable_math"
echo "  shell:     line editing $with_readline"
echo "  tcl:       package=$enable_tcl testfixture=$enable_test"
echo "  debug:     $enable_debug  diagnostic=$enable_diagnostic asan=$enable_asan"
