/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (c) 2007-2026  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * tf_stubs.c --
 *	Tcl init functions for upstream test modules dbsql does not build:
 *	RBU, recover and intck (not shipped), and the b-tree internals test
 *	test_btree.c (native storage only).  test_tclsh.c registers them
 *	unconditionally; they register nothing.  Not a generated file.
 */

#include <tcl.h>

int Dbsqltestintck_Init(Tcl_Interp *);
int DbsqlRbu_Init(Tcl_Interp *);
int __test_recover_init(Tcl_Interp *);

int
Dbsqltestintck_Init(Tcl_Interp *interp)
{
	(void)interp;
	return (TCL_OK);
}

int
DbsqlRbu_Init(Tcl_Interp *interp)
{
	(void)interp;
	return (TCL_OK);
}

int
__test_recover_init(Tcl_Interp *interp)
{
	(void)interp;
	return (TCL_OK);
}
