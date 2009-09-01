/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
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
 * A TCL Shell for testing DBSQL.
 */

#include "dbsql_config.h"

#include "dbsql_int.h"
#include "tcl.h"

#include <stdlib.h>
#include <string.h>

static char main_loop[] =
  "set line {}\n"
  "while {![eof stdin]} {\n"
    "if {$line!=\"\"} {\n"
      "puts -nonewline \"> \"\n"
    "} else {\n"
      "puts -nonewline \"% \"\n"
    "}\n"
    "flush stdout\n"
    "append line [gets stdin]\n"
    "if {[info complete $line]} {\n"
      "if {[catch {uplevel #0 $line} result]} {\n"
        "puts stderr \"Error: $result\"\n"
      "} elseif {$result!=\"\"} {\n"
        "puts $result\n"
      "}\n"
      "set line {}\n"
    "} else {\n"
      "append line \\n\n"
    "}\n"
  "}\n"
;

/*
 * main --
 */
int
main(argc, argv)
	int argc;
	char **argv;
{
	int i;
	const char *info;
	Tcl_Interp *interp;
	Tcl_FindExecutable(argv[0]);
	interp = Tcl_CreateInterp();
	dbsql_init_tcl_interface(interp);
#ifdef CONFIG_TEST
	extern int Sqlitetest1_Init(Tcl_Interp*);
	extern int Sqlitetest2_Init(Tcl_Interp*);
	extern int Sqlitetest3_Init(Tcl_Interp*);
	extern int Sqlitetest4_Init(Tcl_Interp*);
	extern int Md5_Init(Tcl_Interp*);
	Sqlitetest1_Init(interp);
	Sqlitetest2_Init(interp);
	Sqlitetest3_Init(interp);
	Sqlitetest4_Init(interp);
	Md5_Init(interp);
#endif
	if (argc >= 2) {
		Tcl_SetVar(interp, "argv0", argv[1], TCL_GLOBAL_ONLY);
		Tcl_SetVar(interp, "argv", "", TCL_GLOBAL_ONLY);
		for (i = 2; i < argc; i++) {
			Tcl_SetVar(interp, "argv", argv[i],
				   TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT |
				   TCL_APPEND_VALUE);
		}
		if (Tcl_EvalFile(interp, argv[1]) != TCL_OK) {
			info = Tcl_GetVar(interp, "errorInfo",
					  TCL_GLOBAL_ONLY);
			if (info == 0)
				info = interp->result;
			fprintf(stderr, "%s: %s\n", *argv, info);
			return 1;
		}
	} else {
		Tcl_GlobalEval(interp, main_loop);
	}
	return 0;
}
