"""
fixups -- explicit, documented text edits applied after renaming.

Each entry is (dbsql path regex, pattern, replacement, reason).  The
pattern is a Python regex applied with re.M.  A fixup that matches nothing
is an error: when upstream changes, the fixup must be revisited.

Prefer fixups to patches for small mechanical edits that must survive
upstream churn; use dist/transpile/patches/ for real code changes.
"""

FIXUPS = [
	# --- fixed-width integer types --------------------------------------
	# sqlite.h.in defines sqlite_int64/sqlite_uint64 itself; dbsql uses the
	# platform's int64_t/u_int64_t (as libdb does), so the typedef chain
	# collapses to self-typedefs that clash with <sys/types.h>.
	(r'(^|/)(dbsql\.in|dbsql_api_ref\.h)$',
	 r'^\s*typedef\s+(?:unsigned\s+)?(?:DBSQL_INT64_TYPE|DBSQL_UINT64_TYPE|'
	 r'__int64|unsigned __int64|long long int|unsigned long long int|'
	 r'int64_t|u_int64_t)\s+(?:int64_t|u_int64_t)\s*;[^\n]*$',
	 '', 'drop int64 typedef chain; platform provides int64_t/u_int64_t'),
	(r'(^|/)(dbsql\.in|dbsql_api_ref\.h)$',
	 r'^(#include <stdarg\.h>[^\n]*)$',
	 r'\1\n#include <sys/types.h>\t/* Needed for int64_t, u_int64_t */\n'
	 r'#include <stdint.h>',
	 'public header supplies the fixed-width types it uses, as db.h does'),
	(r'(^|/)(dbsql_int\.h)$',
	 r'^typedef\s+(?:int64_t|u_int64_t|UINT32_TYPE|UINT16_TYPE|INT16_TYPE|'
	 r'UINT8_TYPE|INT8_TYPE)\s+(?:int64_t|u_int64_t|u_int32_t|u_int16_t|'
	 r'int16_t|u_int8_t|int8_t)\s*;[^\n]*$',
	 '', 'drop u8..i64 typedefs; platform provides u_int8_t..int64_t'),
	(r'(^|/)(dbsql_int\.h)$',
	 r'^(#include "dbsql_limit\.h")$',
	 r'\1\n#include <sys/types.h>\t\t/* u_int8_t ... u_int64_t */',
	 'libdb-style fixed-width types come from <sys/types.h>'),
	(r'(^|/)fts5_unicode2\.c$',
	 r'\A',
	 '#include <sys/types.h>\n',
	 'fts5_unicode2.c is compiled standalone and uses u_int8_t..u_int32_t'),
	# --- token pasting ---------------------------------------------------
	# window.c builds names as name##StepFunc; the pasted functions were
	# renamed to __<name>_step_func.  Rewrite the paste to match.
	# (fixups see renamed text: StepFunc has become __step_func)
	(r'(^|/)cg_window\.c$',
	 r'\bname ## __(step|finalize|value|inv)_func\b',
	 r'__ ## name ## _\1_func',
	 'window.c token pasting: name##StepFunc -> __##name##_step_func'),
	(r'(^|/)cg_window\.c$',
	 r'\bname ## Name\b',
	 '__ ## name ## _name',
	 'window.c token pasting: name##Name -> __##name##_name'),
	# --- reserved-name prefix length ---------------------------------------
	# SQLite reserves names beginning "sqlite_" (7 chars); dbsql reserves
	# "dbsql_" (6 chars).  Every length/offset tied to that prefix moves
	# from 7 to 6, and "sqlite_autoindex_" (17) becomes
	# "dbsql_autoindex_" (16).  Each site is listed so a new upstream site
	# fails loudly (the fixup count check), and test/unit/reserved.test
	# exercises them.
	(r'(^|/)(cg_alter|cg_build|cg_pragma|cg_resolve|cg_trigger)\.c$',
	 r'("dbsql_",\s*)7\b', r'\g<1>6',
	 'reserved prefix: strnicmp(x, "dbsql_", 6)'),
	(r'(^|/)(cg_build|cg_resolve)\.c$',
	 r'(\b\w+\+)7(, &(?:PREFERRED|LEGACY)_(?:TEMP_)?SCHEMA_TABLE\[)7\]',
	 r'\g<1>6\g<2>6]',
	 'reserved prefix: x+6 vs &SCHEMA_TABLE[6]'),
	(r'(^|/)cg_build\.c$',
	 r'(->name\+)7(, "(?:stat|parameters)")', r'\g<1>6\2',
	 'reserved prefix: tab->name+6 for dbsql_stat*/dbsql_parameters'),
	(r'(^|/)cg_build\.c$',
	 r'(if\( IN_SPECIAL_PARSE \) (?:z_)?name\[)7(\]\+\+;)', r'\g<1>6\2',
	 'dbsql_autoindex -> dbsql_butoindex for declare_vtab indexes'),
	(r'(^|/)cg_where\.c$',
	 r'("dbsql_autoindex_",\s*)17\b', r'\g<1>16',
	 'reserved prefix: "dbsql_autoindex_" is 16 chars'),
	(r'(^|/)cg_alter\.c$',
	 r"('dbsql_autoindex_' \|\| %Q \|\| substr\(name,%d\+)18\)",
	 r'\g<1>17)',
	 'RENAME TABLE: dbsql_autoindex_<tab> suffix starts at len(tab)+17'),
	(r'(^|/)cg_analyze\.c$',
	 r'(memcpy\(stat1->name, "dbsql_stat1", )13\)',
	 r'\g<1>12)',
	 'preupdate stat1 name: "dbsql_stat1" + NUL is 12 bytes'),
	(r'(^|/)cg_alter\.c$',
	 r'(z_tab = &p_new->name\[)16(\];  /\* Skip the "dbsql_altertab_" prefix)',
	 r'\g<1>15\2',
	 'ADD COLUMN: "dbsql_altertab_" is 15 chars (upstream skips 16)'),
	(r'(^|/)api\.c$',
	 r'(__str_nicmp\(opt_name, "DBSQL_", )7(\)==0 \) opt_name \+= )7',
	 r'\g<1>6\g<2>6',
	 'compileoption names: strip the 6-char "DBSQL_" prefix'),
	(r'(^|/)api_loadext\.c$',
	 r'(memcpy\(alt_entry, "dbsql_", )8\);', r'\g<1>6);',
	 'load_extension default entry point: dbsql_<name>_init'),
	(r'(^|/)api_loadext\.c$',
	 r'(for\(i_entry=)8(; )', r'\g<1>6\2',
	 'load_extension default entry point: name starts after "dbsql_"'),
	(r'(^|/)test1\.c$',
	 r'(dbsql_strglob\("DBSQL_\*", setting\)==0 \) setting \+= )7;',
	 r'\g<1>6;', 'test harness: strip "DBSQL_"'),
	(r'(^|/)test_malloc\.c$',
	 r'(memcmp\(op_name, "DBSQL_", )7(\)==0 \) op_name \+= )7;',
	 r'\g<1>6\g<2>6;', 'test harness: strip "DBSQL_"'),
	(r'(^|/)test_osinst\.c$',
	 r'"dbsql_ostrace1\.\.\.\.\.", 20\)', '"dbsql_ostrace1......", 20)',
	 'reserved prefix: test harness keeps the 20-byte osinst log magic'),
	# --- storage manager hooks -------------------------------------------
	# The connection keeps the list of its libdb transaction states, one
	# per environment (src/sm/sm.c: __sm_txn_for).  The native validation
	# build never touches it.
	(r'(^|/)inc/dbsql_int\.h$',
	 r'^(  busy_handler_t busy_handler;      /\* Busy callback \*/)$',
	 r'\1\n  struct sm_txn *sm_txns;          /* libdb txn state per env (sm/) */',
	 'DBSQL.sm_txns for the libdb storage manager'),
	# Upstream bug: with DBSQL_OMIT_WAL the shm object (and the debug-only
	# __unix_describe_shm, defined in the WAL section) does not exist, but
	# the file-stat JSON writer still calls it.  Guard the call.
	(r'(^|/)os/os_unix\.c$',
	 r'(\n)(      if\( file->shm \)\{\n        dbsql_str_appendall\(str, ",\\"shm\\":"\);\n        __unix_describe_shm\(str, file->shm\);\n      \})',
	 r'\1#ifndef DBSQL_OMIT_WAL\n\2\n#endif',
	 'os_unix.c: describe the shm object only when WAL is compiled in'),
	# test2.c tests the native pager; the libdb build keeps only its
	# storage-independent commands (pending byte, fault simulator, I/O
	# error variables, bitvec, fake_big_file).  DBSQL_SM_LIBDB is defined
	# by the libdb build.
	(r'(^|/)test/tcl/test2\.c$',
	 r'(\n/\*\n\*\* Usage:   pager_open FILENAME N-PAGE\n)',
	 r'\n#ifndef DBSQL_SM_LIBDB\1',
	 'test2.c: compile the pager commands out of the libdb build (begin)'),
	(r'(^|/)test/tcl/test2\.c$',
	 r'(\n)(#ifndef DBSQL_OMIT_DISKIO\n/\*\n\*\* Usage:   fake_big_file)',
	 r'\1#endif /* !DBSQL_SM_LIBDB */\n\n\2',
	 'test2.c: compile the pager commands out of the libdb build (end)'),
	(r'(^|/)test/tcl/test2\.c$',
	 r'(  \} cmds\[\] = \{\n)(    \{ "pager_open",[\s\S]*?\n    \{ "pager_truncate", [^\n]*\n)',
	 r'\1#ifndef DBSQL_SM_LIBDB\n\2#endif\n',
	 'test2.c: drop the pager commands from the command table'),
	# test3.c (btree_open, btree_from_db, btree_set_cache_size ...) uses
	# the sm.h API plus sm_t.db; on libdb that struct is in sm_int.h.
	(r'(^|/)test/tcl/test3\.c$',
	 r'^#include "btree_int\.h"\n',
	 r'#ifdef DBSQL_SM_LIBDB\n#include "sm_int.h"\n#else\n#include "btree_int.h"\n#endif\n',
	 'test3.c: sm_int.h instead of btree_int.h on libdb'),
	# --- the shell -------------------------------------------------------
	# The shell sniffs a file's first 16 bytes for the SQLite header to
	# decide how to open it.  A dbsql database is a libdb file, which
	# __is_database_file() recognises through the storage manager; the
	# SQLite magic test can never match a dbsql database.
	(r'(^|/)shell/dbsql\.c$',
	 r'n==1 && memcmp\(buf, "SQLite format 3", 16\)==0',
	 '0 /* dbsql: libdb files are recognised by __is_database_file() */',
	 'shell: no SQLite-format header sniffing'),
]


def apply(path, text):
	import re
	used = []
	for i, (prx, pat, rep, why) in enumerate(FIXUPS):
		if not re.search(prx, path):
			continue
		new, n = re.subn(pat, rep, text, flags=re.M)
		if n == 0:
			raise RuntimeError('fixup %d (%s) matched nothing in %s' %
			    (i, why, path))
		text = new
		used.append(i)
	return text, used
