# $Id: types.ac 7 2007-02-03 13:34:17Z gburd $

# Check the sizes we know about, and see if any of them match what's needed.
#
# Prefer ints to anything else, because read, write and others historically
# returned an int.
AC_DEFUN(AM_SEARCH_USIZES, [
	case "$3" in
	"$ac_cv_sizeof_unsigned_int")
		$1="typedef unsigned int $2;";;
	"$ac_cv_sizeof_unsigned_char")
		$1="typedef unsigned char $2;";;
	"$ac_cv_sizeof_unsigned_short")
		$1="typedef unsigned short $2;";;
	"$ac_cv_sizeof_unsigned_long")
		$1="typedef unsigned long $2;";;
	*)
		AC_MSG_ERROR([No unsigned $3-byte integral type]);;
	esac])
AC_DEFUN(AM_SEARCH_SSIZES, [
	case "$3" in
	"$ac_cv_sizeof_int")
		$1="typedef int $2;";;
	"$ac_cv_sizeof_char")
		$1="typedef char $2;";;
	"$ac_cv_sizeof_short")
		$1="typedef short $2;";;
	"$ac_cv_sizeof_long")
		$1="typedef long $2;";;
	*)
		AC_MSG_ERROR([No signed $3-byte integral type]);;
	esac])

# Check for the standard system types.
AC_DEFUN(AM_TYPES, [

# db.h includes <sys/types.h> and <stdio.h>, not the other default includes
# autoconf usually includes.  For that reason, we specify a set of includes
# for all type checking tests. [#5060]
#
# IBM's OS/390 and z/OS releases have types in <inttypes.h> not also found
# in <sys/types.h>.  Include <inttypes.h> as well, if it exists.
AC_SUBST(inttypes_decl)
db_includes="#include <sys/types.h>"
AC_CHECK_HEADER(inttypes.h, [
	inttypes_decl="#include <inttypes.h>"
	db_includes="$db_includes
#include <inttypes.h>"])
db_includes="$db_includes
#include <stdio.h>"

# We need to know the sizes of various objects on this system.
# We don't use the SIZEOF_XXX values created by autoconf.
AC_CHECK_SIZEOF(char,, $db_includes)
AC_CHECK_SIZEOF(unsigned char,, $db_includes)
AC_CHECK_SIZEOF(short,, $db_includes)
AC_CHECK_SIZEOF(unsigned short,, $db_includes)
AC_CHECK_SIZEOF(int,, $db_includes)
AC_CHECK_SIZEOF(unsigned int,, $db_includes)
AC_CHECK_SIZEOF(long,, $db_includes)
AC_CHECK_SIZEOF(unsigned long,, $db_includes)
AC_CHECK_SIZEOF(size_t,, $db_includes)
AC_CHECK_SIZEOF(char *,, $db_includes)
AC_CHECK_SIZEOF(long double,, $db_includes)

# We require off_t and size_t, and we don't try to substitute our own
# if we can't find them.
AC_CHECK_TYPE(off_t,, AC_MSG_ERROR([No off_t type.]), $db_includes)
AC_CHECK_TYPE(size_t,, AC_MSG_ERROR([No size_t type.]), $db_includes)

# We look for u_char, u_short, u_int, u_long -- if we can't find them,
# we create our own.
AC_SUBST(u_char_decl)
AC_CHECK_TYPE(u_char,,
    [u_char_decl="typedef unsigned char u_char;"], $db_includes)

AC_SUBST(u_short_decl)
AC_CHECK_TYPE(u_short,,
    [u_short_decl="typedef unsigned short u_short;"], $db_includes)

AC_SUBST(u_int_decl)
AC_CHECK_TYPE(u_int,,
    [u_int_decl="typedef unsigned int u_int;"], $db_includes)

AC_SUBST(u_long_decl)
AC_CHECK_TYPE(u_long,,
    [u_long_decl="typedef unsigned long u_long;"], $db_includes)

AC_SUBST(u_int8_decl)
AC_CHECK_TYPE(u_int8_t,,
    [AM_SEARCH_USIZES(u_int8_decl, u_int8_t, 1)], $db_includes)

AC_SUBST(u_int16_decl)
AC_CHECK_TYPE(u_int16_t,,
    [AM_SEARCH_USIZES(u_int16_decl, u_int16_t, 2)], $db_includes)

AC_SUBST(int16_decl)
AC_CHECK_TYPE(int16_t,,
    [AM_SEARCH_SSIZES(int16_decl, int16_t, 2)], $db_includes)

AC_SUBST(u_int32_decl)
AC_CHECK_TYPE(u_int32_t,,
    [AM_SEARCH_USIZES(u_int32_decl, u_int32_t, 4)], $db_includes)

AC_SUBST(int32_decl)
AC_CHECK_TYPE(int32_t,,
    [AM_SEARCH_SSIZES(int32_decl, int32_t, 4)], $db_includes)

AC_SUBST(long_double_decl)
AC_CHECK_TYPE(long double,
    [long_double_decl="typedef long double long_double_t;"],
    [long_double_decl="typedef long double long_double_t;"], $db_includes)

# Check for ssize_t -- if none exists, find a signed integral type that's
# the same size as a size_t.
AC_SUBST(ssize_t_decl)
AC_CHECK_TYPE(ssize_t,,
    [AM_SEARCH_SSIZES(ssize_t_decl, ssize_t, $ac_cv_sizeof_size_t)],
    $db_includes)

# Find the largest integral type.
AC_SUBST(db_align_t_decl)
AC_CHECK_TYPE(unsigned long long,
    [db_align_t_decl="typedef unsigned long long db_align_t;"],
    [db_align_t_decl="typedef unsigned long db_align_t;"], $db_includes)

# Find an integral type which is the same size as a pointer.
AC_SUBST(db_alignp_t_decl)
AM_SEARCH_USIZES(db_alignp_t_decl, db_alignp_t, $ac_cv_sizeof_char_p)
])
