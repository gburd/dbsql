# DBSQL - A SQL database engine.
#
# Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
#
# This library is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# There are special exceptions to the terms and conditions of the GPL as it
# is applied to this software. View the full text of the exception in file
# LICENSE_EXCEPTIONS in the directory of this software distribution.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.

# This awk script parses C input files looking for lines marked "PUBLIC:"
# and "EXTERN:".  (PUBLIC lines are DB internal function prototypes and
# #defines, EXTERN are DB external function prototypes and #defines.)
#
# PUBLIC lines are put into two versions of per-directory include files:
# one file that contains the prototypes, and one file that contains a
# #define for the name to be processed during configuration when creating
# unique names for every global symbol in the DB library.
#
# The EXTERN lines are put into two files: one of which contains prototypes
# which are always appended to the db.h file, and one of which contains a
# #define list for use when creating unique symbol names.
#
# Four arguments:
#	e_dfile		list of EXTERN #defines
#	e_pfile		include file that contains EXTERN prototypes
#	i_dfile		list of internal (PUBLIC) #defines
#	i_pfile		include file that contains internal (PUBLIC) prototypes
/PUBLIC:/ {
	sub("^.*PUBLIC:[	 ][	 ]*", "")
	if ($0 ~ "^#if|^#ifdef|^#ifndef|^#else|^#endif") {
		print $0 >> i_pfile
		print $0 >> i_dfile
		next
	}
	pline = sprintf("%s %s", pline, $0)
	if (pline ~ "\\)\\);") {
		sub("^[	 ]*", "", pline)
		print pline >> i_pfile
		pline = ""
	}
}

/EXTERN:/ {
	sub("^.*EXTERN:[	 ][	 ]*", "")
	if ($0 ~ "^#if|^#ifdef|^#ifndef|^#else|^#endif") {
		print $0 >> e_pfile
		print $0 >> e_dfile
		next
	}
	eline = sprintf("%s %s", eline, $0)
	if (eline ~ "\\)\\);") {
		sub("^[	 ]*", "", eline)
		print eline >> e_pfile
		eline = ""
	}
}
