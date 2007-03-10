# $Id: Makefile.in 7 2007-02-03 13:34:17Z gburd $

srcdir=	./
tstdir=	./tests
platform=unix
builddir=../../build_$(platform)

##################################################
# Installation directories and permissions.
##################################################
prefix=	/usr/local/DBSQL.0.2
exec_prefix=${prefix}
bindir=	${exec_prefix}/bin
includedir=${prefix}/include
libdir=	${exec_prefix}/lib

dmode=	755
emode=	555
fmode=	444

transform=s,x,x,

##################################################
# Paths for standard user-level commands.
##################################################
SHELL=	/bin/sh
ar=	ar
awk=	awk
chmod=	chmod
cp=	cp
depend= makedepend
grep=	grep
ln=	ln
mkdir=	mkdir
ranlib=	/usr/bin/ranlib
rm=	rm
rpm=	
sed=	sed
splint= splint
strip=	debug_build_no_strip

##################################################
# General library information.
##################################################
DEF_LIB=	$(libso_target)
DEF_LIB_CXX=	
INSTALLER=	$(LIBTOOL) --mode=install cp -p
LIBTOOL=	$(SHELL) $(builddir)/libtool


POSTLINK=	$(LIBTOOL) --mode=execute true
SOLINK=		$(LIBTOOL) --mode=link cc -avoid-version
SOFLAGS=	-rpath $(libdir)
LIBMAJOR=	0
LIBVERSION=	0.2

CPPFLAGS=	-I$(builddir) -I$(srcdir) -I../../src \
			-I/usr/include/tcl8.4 \
			 -I/usr/local/db/include -D_GNU_SOURCE -D_REENTRANT

##################################################
# C API.
##################################################
CFLAGS=	 	-c $(CPPFLAGS)  -DCONFIG_TEST -g
CC=		$(LIBTOOL) --mode=compile cc
CCLINK=		$(LIBTOOL) --mode=link cc
LDFLAGS=	  -Wl,--export-dynamic
LIBS=		 -lm -ldl  -lpthread -lieee -lm -L/usr/lib -ltcl8.4${TCL_DBGX}  -L/usr/local/db/lib -ldb-4.5

##################################################
# TCL testing harness.
##################################################
libtso_base=	libdbsql_tcl
libtso=		$(libtso_base)-$(LIBVERSION).so
libtso_static=	$(libtso_base)-$(LIBVERSION).a
libtso_target=	$(libtso_base)-$(LIBVERSION).la
libtso_default=	$(libtso_base).so
libtso_major=	$(libtso_base)-$(LIBMAJOR).so

##################################################
# NOTHING BELOW THIS LINE SHOULD EVER NEED TO BE MODIFIED.
##################################################

##################################################
# Source file lists.
##################################################

TCL_FILES=\
	$(scrdir)/tcl_internal.c \
	$(scrdir)/tcl_dbsql.c $(scrdir)/tcl_printf.c \
	$(scrdir)/tcl_randstr.c $(scrdir)/tcl_sql_funcs.c \
	$(scrdir)/tcl_test_sh.c $(scrdir)/tcl_threads.c \
	$(scrdir)/tcl_md5.c

TCL_OBJS=\
	tcl_md5.lo tcl_printf.lo tcl_randstr.lo tcl_threads.lo \
	tcl_sql_funcs.lo tcl_internal.lo \
	tcl_dbsql.lo 

##################################################
# Note: "all" must be the first target in the Makefile.
##################################################
all: library_build dbsql_tclsh

##################################################
# Library and standard utilities build.
##################################################
library_build: $(libso_target) $(libdb) $(libtso_target)  $(UTIL_PROGS)

# Shared Tcl library.
$(libtso_target): $(builddir)/dbsql_int.h $(builddir)/sql_parser.h $(builddir)/opcodes.h $(TCL_OBJS) $(C_OBJS)
	$(SOLINK) -module $(SOFLAGS) $(LDFLAGS) \
	    -o $@ $(TCL_OBJS) $(C_OBJS)

##################################################
# Utilities
##################################################

dbsql_tclsh: $(libtso_target) dbsql_tclsh.lo
	$(CCLINK) -o $@ $(LDFLAGS) dbsql_tclsh.lo\
	    .libs/$(libtso_static)\
	    -L../../build_unix -ldbsql-0.2\
	    $(LIBS)
	$(POSTLINK) $@

##################################################
# Testing.
##################################################
test: dbsql_tclsh
	./dbsql_tclsh $(tstdir)/quick.test

##################################################
# Remaining standard Makefile targets.
##################################################
CLEAN_LIST=\
	dbsql_tclsh

mostly-clean clean:
	$(rm) -rf $(C_OBJS)
	$(rm) -rf $(TCL_OBJS)
	$(rm) -rf dbsql_tclsh
	$(rm) -rf *.o *.lo *.loT *.la
	$(rm) -rf test.db test?.db test.db-journal test.tcl test1.bt data?.txt

REALCLEAN_LIST=\
	Makefile

##################################################
# Tcl API build rules.
##################################################
tcl_dbsql.lo: $(srcdir)/tcl_dbsql.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_dbsql_pkg.lo: $(srcdir)/tcl_dbsql_pkg.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_internal.lo: $(srcdir)/tcl_internal.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_md5.lo: $(srcdir)/tcl_md5.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_printf.lo: $(srcdir)/tcl_printf.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_randstr.lo: $(srcdir)/tcl_randstr.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_sql_funcs.lo: $(srcdir)/tcl_sql_funcs.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?
tcl_threads.lo: $(srcdir)/tcl_threads.c
	$(CC) $(CFLAGS) $(TCL_CFLAGS) $?

##################################################
# Utility build rules.
##################################################

dbsql_tclsh.lo: $(srcdir)/dbsql_tclsh.c
	$(CC) $(CFLAGS) $?

