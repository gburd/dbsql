# DBSQL - A SQL database engine.
#
# Copyright (C) 2007  The DBSQL Group, Inc.
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
