# DBSQL - A SQL database engine.
#
# Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# There are special exceptions to the terms and conditions of the GPL as it
# is applied to this software. View the full text of the exception in file
# LICENSE_EXCEPTIONS in the directory of this software distribution.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# http://creativecommons.org/licenses/GPL/2.0/
#
# $Id: configure.ac 7 2007-02-03 13:34:17Z gburd $

# Process this file with autoconf to produce a configure script.

PACKAGE=dbsql
AC_INIT(DBSQL,
	__EDIT_DBSQL_VERSION__,
	support@dbsql.org,
	dbsql-__EDIT_DBSQL_VERSION__)
AC_CONFIG_SRCDIR([../src/clib/memcmp.c])
AC_CONFIG_HEADERS([dbsql_config.h:config.hin])

# Configure setup.
AC_CANONICAL_HOST()
AC_ARG_PROGRAM()

# Don't build in the top-level or dist directories.
AC_MSG_CHECKING(if building in the top-level or dist directories)
if [ test -d build_unix -o -d aclocal ] ; then
	AC_MSG_RESULT(yes)
	AC_MSG_ERROR(
    [DBSQL should not be built in the top-level or dist directories.])
fi
AC_MSG_RESULT(no)

# Substitution variables.
AC_SUBST(ADDITIONAL_INCS)
AC_SUBST(ADDITIONAL_LANG)
AC_SUBST(ADDITIONAL_OBJS)
AC_SUBST(ADDITIONAL_PROGS)
AC_SUBST(BUILD_TARGET)
AC_SUBST(CFLAGS)
AC_SUBST(CONFIGURATION_ARGS)
AC_SUBST(CONFIGURATION_PATH)
AC_SUBST(CPPFLAGS)
AC_SUBST(CXX)
AC_SUBST(CXXFLAGS)
AC_SUBST(DB_PATH)
AC_SUBST(DB_UNIQUE_NAME)
AC_SUBST(DEFAULT_LIB)
AC_SUBST(DEFAULT_LIB_CXX)
AC_SUBST(ENCODING)
AC_SUBST(INSTALLER)
AC_SUBST(INSTALL_LIBS)
AC_SUBST(INSTALL_TARGET)
AC_SUBST(LDFLAGS)
AC_SUBST(LIBS)
AC_SUBST(LIBSO_LIBS)
AC_SUBST(LIBTOOL)
AC_SUBST(LIBTSO_LIBS)
AC_SUBST(LIBTSO_MODSUFFIX)
AC_SUBST(LIBTSO_MODULE)
AC_SUBST(LIBXSO_LIBS)
AC_SUBST(LOAD_LIBS)
AC_SUBST(MAKEFILE_CC)
AC_SUBST(MAKEFILE_CCLINK)
AC_SUBST(MAKEFILE_CXX)
AC_SUBST(MAKEFILE_CXXLINK)
AC_SUBST(MAKEFILE_SOLINK)
AC_SUBST(MAKEFILE_XSOLINK)
AC_SUBST(OSDIR)
AC_SUBST(PATH_SEPARATOR)
AC_SUBST(POSTLINK)
AC_SUBST(REPLACEMENT_OBJS)
AC_SUBST(RPC_CLIENT_OBJS)
AC_SUBST(RPM_BUILD)
AC_SUBST(RPM_POST_INSTALL)
AC_SUBST(RPM_POST_UNINSTALL)
AC_SUBST(SOFLAGS)
AC_SUBST(o)

# RPM needs the current absolute path.
# RPM needs the list of original arguments, but we don't include the RPM
# option itself.
CONFIGURATION_PATH=${PWD-`pwd`}
CONFIGURATION_ARGS=`echo "$*" | sed -e 's/--with-rpm[[^ ]]*//'`

# Set the default installation location.
AC_PREFIX_DEFAULT(/usr/local/DBSQL.__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__)

# Configure the version information.
AC_SUBST(DBSQL_VERSION_MAJOR)
DBSQL_VERSION_MAJOR="__EDIT_DBSQL_VERSION_MAJOR__"
AC_SUBST(DBSQL_VERSION_MINOR)
DBSQL_VERSION_MINOR="__EDIT_DBSQL_VERSION_MINOR__"
AC_SUBST(DBSQL_VERSION_PATCH)
DBSQL_VERSION_PATCH="__EDIT_DBSQL_VERSION_PATCH__"
AC_SUBST(DBSQL_VERSION_STRING)
DBSQL_VERSION_STRING='"__EDIT_DBSQL_VERSION_STRING__"'
AC_SUBST(DBSQL_VERSION_UNIQUE_NAME)

# Process all options before using them.
AM_OPTIONS_SET

# Set some #defines based on configuration options.
if test "$db_cv_diagnostic" = "yes"; then
	AC_DEFINE(DIAGNOSTIC)
	AH_TEMPLATE(DIAGNOSTIC,
    [Define to 1 if you want a version with run-time diagnostic checking.])
fi
if test "$db_cv_test" = "yes"; then
	AC_DEFINE(CONFIG_TEST)
	AH_TEMPLATE(CONFIG_TEST,
    [Define to 1 if you want to build a version for running the test suite.])
fi

# Check for programs used in building and installation.
AM_PROGRAMS_SET
AC_PROG_INSTALL

# RPM support: change the standard make and install targets
if test "$db_cv_rpm" = "yes"; then
	BUILD_TARGET="rpm_build"

	# Check if we are running RPM version 3 or 4.
	case "`rpm --version`" in
	*version\ 4*)
		RPM_BUILD="rpmbuild"
		echo "_topdir $CONFIGURATION_PATH" > rpm-macro-defines;;
	*version\ 3*)
		RPM_BUILD="rpm"
		echo "topdir: $CONFIGURATION_PATH" > rpm-macro-defines;;
	esac
	INSTALL_TARGET="rpm_install"
else
	BUILD_TARGET="library_build"
	INSTALL_TARGET="library_install"
fi

# This is where we handle stuff that autoconf can't handle: compiler,
# preprocessor and load flags, libraries that the standard tests don't
# look for.  The default optimization is -O.  We would like to set the
# default optimization for systems using gcc to -O2, but we can't.  By
# the time we know we're using gcc, it's too late to set optimization
# flags.
#
# There are additional libraries we need for some compiler/architecture
# combinations.
#
# Some architectures require DB to be compiled with special flags and/or
# libraries for threaded applications
#
# The makefile CC may be different than the CC used in config testing,
# because the makefile CC may be set to use $(LIBTOOL).
#
# XXX
# Don't override anything if it's already set from the environment.
optimize_def="-O"
case "$host_os" in
aix4.3.*|aix5*)
	optimize_def="-O2"
	CC=${CC-"xlc_r"}
	CPPFLAGS="$CPPFLAGS -D_THREAD_SAFE"
	LDFLAGS="$LDFLAGS -Wl,-brtl";;
bsdi3*)	optimize_def="-O2"
	CC=${CC-"shlicc2"}
	LIBS="$LIBS -lipc";;
bsdi*)	optimize_def="-O2";;
cygwin*)
	optimize_def="-O2"
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE -D_REENTRANT";;
freebsd*)
	optimize_def="-O2"
	CPPFLAGS="$CPPFLAGS -D_THREAD_SAFE"
	LDFLAGS="$LDFLAGS -pthread";;
gnu*|k*bsd*-gnu|linux*)
	optimize_def="-O2"
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE -D_REENTRANT";;
hpux*)	CPPFLAGS="$CPPFLAGS -D_REENTRANT";;
irix*)	optimize_def="-O2"
	CPPFLAGS="$CPPFLAGS -D_SGI_MP_SOURCE";;
mpeix*)	CPPFLAGS="$CPPFLAGS -D_POSIX_SOURCE -D_SOCKET_SOURCE"
	LIBS="$LIBS -lsocket -lsvipc";;
osf*)	CPPFLAGS="$CPPFLAGS -pthread";;
*qnx*)	AC_DEFINE(HAVE_QNX)
	AH_TEMPLATE(HAVE_QNX, [Define to 1 if building on QNX.]);;
solaris*)
	CPPFLAGS="$CPPFLAGS -D_REENTRANT";;
esac

# Set CFLAGS/CXXFLAGS.  We MUST set the flags before we call autoconf
# compiler configuration macros, because if we don't, they set CFLAGS
# to no optimization and -g, which isn't what we want.
if test "$db_cv_debug" = "no"; then
    CFLAGS=${CFLAGS-$optimize_def}
fi
CXXFLAGS=${CXXFLAGS-"$CFLAGS"}

# If the user wants a TCL library interface setup for testing, add
# -DCONFIG_TEST to the CFLAGS value.
if test "$db_cv_test" = "yes"; then
	AC_DEFINE(CONFIG_TEST)
	AH_TEMPLATE(CONFIG_TEST,
		[Define to 1 if you want a TCL API with test support.])

	CFLAGS="$CFLAGS -DCONFIG_TEST"
	CXXFLAGS="$CXXFLAGS -DCONFIG_TEST"
	db_cv_build_tcl_test_objs=TCL_TEST_OBJS
else
	db_cv_build_tcl_test_objs=
fi
AC_SUBST(db_cv_build_tcl_test_objs)


# If the user wants a debugging environment, add -g to the CFLAGS value.
#
# XXX
# Some compilers can't mix optimizing and debug flags.  The only way to
# handle this is to specify CFLAGS in the environment before configuring.
if test "$db_cv_debug" = "yes"; then
	AC_DEFINE(DEBUG)
	AH_TEMPLATE(DEBUG, [Define to 1 if you want a debugging version.])

	CFLAGS="$CFLAGS -g"
	CXXFLAGS="$CXXFLAGS -g"
	db_cv_build_type=debug
else
	db_cv_build_type=release
fi
AC_SUBST(db_cv_build_type)

# The default compiler is cc (NOT gcc), the default CFLAGS is as specified
# above, NOT what is set by AC_PROG_CC, as it won't set optimization flags
# for any compiler other than gcc.
AC_PROG_CC(cc gcc)

# Checks for compiler characteristics.
AC_SUBST(DB_PROTO1)
AC_SUBST(DB_PROTO2)

DB_PROTO1="#undef __P"

# AC_PROG_CC_STDC only sets ac_cv_prog_cc_stdc if the test fails, so
# check for "no", not "yes".
if test "$ac_cv_prog_cc_stdc" = "no"; then
	DB_PROTO2="#define	__P(protos)	()"
else
	DB_PROTO2="#define	__P(protos)	protos"
fi

AC_C_CONST
AC_SUBST(DB_CONST)
if test "$ac_cv_c_const" != "yes"; then
	DB_CONST="#define const"
fi

# Because of shared library building, the ${CC} used for config tests
# may be different than the ${CC} we want to put in the Makefile.
# The latter is known as ${MAKEFILE_CC} in this script.
MAKEFILE_CC="${CC}"
MAKEFILE_CCLINK="${CC}"
MAKEFILE_CXX="nocxx"
MAKEFILE_CXXLINK="nocxx"

# See if we need the C++ compiler at all.  If so, we'd like to find one that
# interoperates with the C compiler we chose.  Since we prefered cc over gcc,
# we'll also prefer the vendor's compiler over g++/gcc.  If we're wrong, the
# user can set CC and CXX in their environment before running configure.
#
# AC_PROG_CXX sets CXX, but it uses $CXX and $CCC (in that order) as its
# first choices.
if test "$db_cv_cxx" = "yes"; then
	if test "$GCC" != "yes"; then
		case "$host_os" in
		aix*)		AC_CHECK_TOOL(CCC, xlC_r)
				LIBXSO_LIBS="-lC_r $LIBXSO_LIBS"
				LIBS="-lC_r $LIBS";;
		hpux*)		AC_CHECK_TOOL(CCC, aCC);;
		irix*)		AC_CHECK_TOOL(CCC, CC);;
		osf*)		AC_CHECK_TOOL(CCC, cxx);;
		solaris*)	AC_CHECK_TOOL(CCC, CC);;
		esac
	fi
	AC_PROG_CXX
	###### WORKAROUND: SEE SR #7938
	AC_PROG_CXXCPP
	###############################
	AC_CXX_HAVE_STDHEADERS
	MAKEFILE_CXX="${CXX}"
	MAKEFILE_CXXLINK="${CXX}"
fi

# Do some gcc specific configuration.
AC_GCC_CONFIG1
AC_GCC_CONFIG2

# We need the -Kthread/-pthread flag when compiling on SCO/Caldera's UnixWare
# and OpenUNIX releases.  We can't make the test until we know which compiler
# we're using.
case "$host_os" in
sysv5UnixWare*|sysv5OpenUNIX8*)
	if test "$GCC" == "yes"; then
		CPPFLAGS="$CPPFLAGS -pthread"
		LDFLAGS="$LDFLAGS -pthread"
	else
		CPPFLAGS="$CPPFLAGS -Kthread"
		LDFLAGS="$LDFLAGS -Kthread"
	fi;;
esac

# Export our compiler preferences for the libtool configuration.
export CC CCC
CCC=CXX

# Libtool configuration.
AC_PROG_LIBTOOL

SOFLAGS="-rpath \$(libdir)"
LIBTOOL_PROG="${SHELL} ./libtool"

# Set SOSUFFIX and friends
SOSUFFIX_CONFIG
MODSUFFIX_CONFIG

INSTALLER="\$(LIBTOOL) --mode=install cp -p"

MAKEFILE_CC="\$(LIBTOOL) --mode=compile ${MAKEFILE_CC}"
MAKEFILE_SOLINK="\$(LIBTOOL) --mode=link ${MAKEFILE_CCLINK} -avoid-version"
MAKEFILE_CCLINK="\$(LIBTOOL) --mode=link ${MAKEFILE_CCLINK}"
MAKEFILE_CXX="\$(LIBTOOL) --mode=compile ${MAKEFILE_CXX}"
MAKEFILE_XSOLINK="\$(LIBTOOL) --mode=link ${MAKEFILE_CXXLINK} -avoid-version"
MAKEFILE_CXXLINK="\$(LIBTOOL) --mode=link ${MAKEFILE_CXXLINK}"

LIBTOOL="\$(SHELL) ./libtool"

case "$host_os" in
cygwin*)
	MAKEFILE_SOLINK="$MAKEFILE_SOLINK -no-undefined"
	MAKEFILE_XSOLINK="$MAKEFILE_XSOLINK -no-undefined";;
esac

# Configure for shared libraries, static libraries, or both.  If both are
# configured, build the utilities and example programs with shared versions.
#
# $o is set to ".o" or ".lo", and is the file suffix used in the Makefile
# instead of .o
if test `$LIBTOOL_PROG --config |
    grep build_libtool_libs | grep no` 2>/dev/null; then
	enable_shared="no"
else
	enable_shared="yes"
fi
if test `$LIBTOOL_PROG --config |
    grep build_old_libs | grep no` 2>/dev/null; then
	enable_static="no"
else
	enable_static="yes"
fi

# C API.
if test "$enable_shared" = "no"; then
	DEFAULT_LIB="\$(libdb_version)"
	POSTLINK=": "
	o=".o"
else
	DEFAULT_LIB="\$(libso_target)"
	POSTLINK="\$(LIBTOOL) --mode=execute true"
	o=".lo"
fi
INSTALL_LIBS="$DEFAULT_LIB"
if test "$enable_static" = "yes"; then
	INSTALL_LIBS="$INSTALL_LIBS \$(libdb)"
fi

# Optional C++ API.
if test "$db_cv_cxx" = "yes"; then
	if test "$enable_shared" = "no"; then
		DEFAULT_LIB_CXX="\$(libcxx_version)"
	fi
	if test "$enable_shared" = "yes"; then
		DEFAULT_LIB_CXX="\$(libxso_target)"
	fi
	INSTALL_LIBS="$INSTALL_LIBS $DEFAULT_LIB_CXX"
	if test "$enable_static" = "yes"; then
		INSTALL_LIBS="$INSTALL_LIBS \$(libcxx)"
	fi
fi

dnl # Optional Java API. TODO

# MinGW support.
if test "$db_cv_mingw" = "yes"; then
	OSDIR=os_win32
	PATH_SEPARATOR="\\\\/:"

	AC_DEFINE(DBSQL_WIN32)
	AC_DEFINE(STDC_HEADERS)
else
	OSDIR=os
	PATH_SEPARATOR="/"
fi

# Apple's Mac OS/X had to rename shared libraries, so much for standards.
case "$host_os" in
    darwin*)
        LIBTSO_MODULE=""
        LIBTSO_MODSUFFIX=".dylib"
    ;;
    *)
        LIBTSO_MODULE="-module"
        LIBTSO_MODSUFFIX=@MODSUFFIX@
    ;;
esac

AM_TCL_LOAD

# You can disable pieces of functionality to save space.

# The stat code.
if test "$db_cv_build_statistics" = "yes"; then
	AC_DEFINE(HAVE_STATISTICS)
	AH_TEMPLATE(HAVE_STATISTICS,
	    [Define to 1 if building statistics support.])
fi

# Checks for include files, structures, C types.
AC_HEADER_STAT
AC_HEADER_TIME
AC_HEADER_DIRENT
AC_CHECK_HEADERS(sys/select.h sys/time.h sys/fcntl.h)
AC_CHECK_MEMBERS([struct stat.st_blksize])
AM_TYPES

AC_CACHE_CHECK([for ANSI C exit success/failure values], db_cv_exit_defines, [
AC_TRY_COMPILE([#include <stdlib.h>], return (EXIT_SUCCESS);,
    [db_cv_exit_defines=yes], [db_cv_exit_defines=no])])
if test "$db_cv_exit_defines" = "yes"; then
	AC_DEFINE(HAVE_EXIT_SUCCESS)
	AH_TEMPLATE(HAVE_EXIT_SUCCESS,
    [Define to 1 if you have EXIT_SUCCESS/EXIT_FAILURE #defines.])
fi

# Test for various functions/libraries that the test and example programs use:
#	sched_yield function
#	pthreads, socket and math libraries
AC_CHECK_FUNC(sched_yield,,
    AC_SEARCH_LIBS(sched_yield, rt, LOAD_LIBS="$LOAD_LIBS -lrt"))

# XXX
# We can't check for pthreads in the same way we did the test for sched_yield
# because the Solaris C library includes pthread interfaces which are not
# thread-safe.  For that reason we always add -lpthread if we find a pthread
# library.  Also we can't depend on any specific call existing (pthread_create,
# for example), as it may be #defined in an include file -- OSF/1 (Tru64) has
# this problem.
AC_HAVE_LIBRARY(pthread, LOAD_LIBS="$LOAD_LIBS -lpthread")

# We use sqrt() so we need the math library -lm
AC_HAVE_LIBRARY(m, LIBS="$LIBS -lm")

# Checks for system functions for which we have replacements.
AC_REPLACE_FUNCS(getopt memcmp memcpy memmove strdup strndup strerror)
AC_REPLACE_FUNCS(snprintf localtime_r strcasecmp strncasecmp)
AC_REPLACE_FUNCS(srand48_r lrand48_r)

# Check for system functions we use.
AC_CHECK_FUNCS(usleep)

# A/UX has a broken getopt(3).
case "$host_os" in
aux*)	AC_LIBOBJ([getopt]);;
esac

# Apple's Mac OS/X complains about long doubles, make it stop.
case "$host_os" in
     darwin*)
	CFLAGS="$CFLAGS -Wno-long-double -no-cpp-precomp"
	;;
esac

# We need to add the additional object files into the Makefile with the correct
# suffix.  We can't use $LTLIBOBJS itself, because that variable has $U encoded
# in it for automake, and that's not what we want.
REPLACEMENT_OBJS=`echo "$LIB@&t@OBJS" |
		 sed "s,\.[[^.]]* ,$o ,g;s,\.[[^.]]*$,$o,"`

# This is necessary so that .o files in LIBOBJS are also built via
# the ANSI2KNR-filtering rules.
LIB@&t@OBJS=`echo "$LIB@&t@OBJS" |
	     sed 's,\.[[^.]]* ,$U&,g;s,\.[[^.]]*$,$U&,'`
LTLIBOBJS=`echo "$LIB@&t@OBJS" |
	   sed 's,\.[[^.]]* ,.lo ,g;s,\.[[^.]]*$,.lo,'`
AC_SUBST(LTLIBOBJS)

# Initial output file list.
CREATE_LIST="Makefile
	dbsql_int.h:$srcdir/../src/inc/dbsql_int.in
	$srcdir/../src/py/setup.py:$srcdir/../src/py/setup.py.in
	$srcdir/../test/scr050/Makefile:$srcdir/../test/scr050/Makefile.in"

# MinGW needs win_db.h.
if test "$db_cv_mingw" = "yes"; then
CREATE_LIST="$CREATE_LIST
    win_db.h:$srcdir/win_dbsql.in"
fi

# Create the dbsql.h file from a source file and a list of global function
# prototypes.
CREATE_LIST="$CREATE_LIST
dbsql.h:$srcdir/../src/dbsql.in"

if test "$db_cv_rpm" = "yes"; then
	CREATE_LIST="$CREATE_LIST db.spec:../dist/dbsql.spec.in"
fi

AC_CONFIG_FILES($CREATE_LIST)
AC_OUTPUT
