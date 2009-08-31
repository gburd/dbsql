# $Id: programs.ac 7 2007-02-03 13:34:17Z gburd $

# Check for programs used in building/installation.
AC_DEFUN(AM_PROGRAMS_SET, [

AC_CHECK_TOOL(db_cv_path_ar, ar, missing_ar)
if test "$db_cv_path_ar" = missing_ar; then
	AC_MSG_ERROR([No ar utility found.])
fi

AC_CHECK_TOOL(db_cv_path_chmod, chmod, missing_chmod)
if test "$db_cv_path_chmod" = missing_chmod; then
	AC_MSG_ERROR([No chmod utility found.])
fi

AC_CHECK_TOOL(db_cv_path_sed, sed, missing_sed)
if test "$db_cv_path_sed" = missing_sed; then
	AC_MSG_ERROR([No sed utility found.])
fi

AC_CHECK_TOOL(db_cv_path_perl, perl, missing_perl)
if test "$db_cv_path_perl" = missing_perl; then
	AC_MSG_ERROR([No perl utility found.])
fi

AC_CHECK_TOOL(db_cv_path_makedepend, makedepend, missing_makedepend)
if test "$db_cv_path_makedepend" = missing_makedepend; then
	AC_MSG_RESULT([no])
	db_cv_path_makedepend=echo
fi

AC_CHECK_TOOL(db_cv_path_splint, splint, missing_splint)
if test "$db_cv_path_splint" = missing_splint; then
	AC_MSG_RESULT([no])
	db_cv_path_splint=echo
fi

AC_CHECK_TOOL(db_cv_path_python, python, missing_python)
if test "$db_cv_path_python" = missing_python; then
	AC_MSG_RESULT([no])
	db_cv_path_python=echo
fi

AC_CHECK_TOOL(db_cv_path_grep, grep, missing_grep)
if test "$db_cv_path_grep" = missing_grep; then
	AC_MSG_ERROR([No grep utility found.])
fi

AC_CHECK_TOOL(db_cv_path_awk, awk, missing_awk)
if test "$db_cv_path_awk" = missing_awk; then
	AC_MSG_ERROR([No awk utility found.])
fi

AC_CHECK_TOOL(db_cv_path_cp, cp, missing_cp)
if test "$db_cv_path_cp" = missing_cp; then
	AC_MSG_ERROR([No cp utility found.])
fi

if test "$db_cv_rpm" = "yes"; then
	AC_CHECK_TOOL(path_ldconfig, ldconfig, missing_ldconfig)
	AC_PATH_PROG(db_cv_path_ldconfig, $path_ldconfig, missing_ldconfig)
	if test "$db_cv_path_ldconfig" != missing_ldconfig; then
		RPM_POST_INSTALL="%post -p $db_cv_path_ldconfig"
		RPM_POST_UNINSTALL="%postun -p $db_cv_path_ldconfig"
	fi
fi

AC_CHECK_TOOL(db_cv_path_ln, ln, missing_ln)
if test "$db_cv_path_ln" = missing_ln; then
	AC_MSG_ERROR([No ln utility found.])
fi

AC_CHECK_TOOL(db_cv_path_mkdir, mkdir, missing_mkdir)
if test "$db_cv_path_mkdir" = missing_mkdir; then
	AC_MSG_ERROR([No mkdir utility found.])
fi

# We need a complete path for ranlib, because it doesn't exist on some
# architectures because the ar utility packages the library itself.
AC_CHECK_TOOL(path_ranlib, ranlib, missing_ranlib)
AC_PATH_PROG(db_cv_path_ranlib, $path_ranlib, missing_ranlib)

AC_CHECK_TOOL(db_cv_path_rm, rm, missing_rm)
if test "$db_cv_path_rm" = missing_rm; then
	AC_MSG_ERROR([No rm utility found.])
fi

if test "$db_cv_rpm" = "yes"; then
	AC_CHECK_TOOL(db_cv_path_rpm, rpm, missing_rpm)
	if test "$db_cv_path_rpm" = missing_rpm; then
		AC_MSG_ERROR([No rpm utility found.])
	fi
fi

# We need a complete path for sh, because some implementations of make
# get upset if SHELL is set to just the command name.
AC_CHECK_TOOL(path_sh, sh, missing_sh)
AC_PATH_PROG(db_cv_path_sh, $path_sh, missing_sh)
if test "$db_cv_path_sh" = missing_sh; then
	AC_MSG_ERROR([No sh utility found.])
fi

# Don't strip the binaries if --enable-debug was specified.
if test "$db_cv_debug" = yes; then
	db_cv_path_strip=debug_build_no_strip
else
	AC_CHECK_TOOL(path_strip, strip, missing_strip)
	AC_PATH_PROG(db_cv_path_strip, $path_strip, missing_strip)
fi

if test "$db_cv_test" = "yes"; then
	AC_CHECK_TOOL(db_cv_path_kill, kill, missing_kill)
	if test "$db_cv_path_kill" = missing_kill; then
		AC_MSG_ERROR([No kill utility found.])
	fi
fi

])
