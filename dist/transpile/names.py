"""
names -- the SQLite -> dbsql identifier mapping.

Every identifier token in a transformed file passes through Renamer.map().
The mapping is a pure function of the token text (plus the optional
per-function overrides in names.map), so the same upstream name always maps
the same way in every file.  Rule order (first match wins):

  1. names.map overrides (global "old new", or "func:old new")
  2. public API: sqlite3_* functions/types, SQLITE_* constants (api_name)
  3. internal API: sqlite3Xxx -> __xxx (internal_name)
  4. upstream typedef names (Table, Expr, ...) -> table_t, expr_t (type_name)
  5. CamelCase macros (ExprHasProperty) -> EXPR_HAS_PROPERTY
  6. Hungarian / camelCase locals and fields -> readable snake_case
  7. anything else: unchanged

See .agent/steering/TRANSFORM.md for the rules in prose.
"""

import keyword, re

KEEP_SQL_NAMES = False	# set by run --keep-sql-names (validation build)

C_KEYWORDS = set('''
auto break case char const continue default do double else enum extern float
for goto if inline int long register restrict return short signed sizeof static
struct switch typedef union unsigned void volatile while _Bool _Complex
_Imaginary _Alignas _Alignof _Atomic _Generic _Noreturn _Static_assert
_Thread_local bool true false asm typeof
class delete new namespace template this throw try catch operator private
protected public virtual friend explicit mutable typename using
'''.split())

# Common libc/POSIX/libdb names a renamed identifier must never become.
RESERVED = C_KEYWORDS | set('''
abs bcmp bzero close errno exit fflush fprintf free fseek ftell index isalnum
isalpha isdigit isspace isupper isxdigit labs localtime log main malloc memchr
memcmp memcpy memmove memset open printf qsort rand read realloc rindex round
select signal sleep snprintf sprintf sqrt stat stderr stdin stdout strcat
strchr strcmp strcpy strcspn strdup strerror strftime strlen strncmp strncpy
strrchr strspn strstr strtod strtol system time tolower toupper unlink write
assert offsetof NULL DB DBT DBC DB_ENV DB_TXN DB_SEQUENCE ENV
stat lstat fstat access pread pwrite sync fsync ftruncate getpid mmap munmap
'''.split())

# Upstream Hungarian prefixes.  'drop' removes the prefix; 'keep' keeps it as
# a separate word (n_col, i_db); 'plural' removes it and pluralises.
PREFIX_RULES = [
	('ap', 'plural'), ('az', 'plural'), ('aa', 'plural'), ('ai', 'plural'),
	('pp', 'drop'), ('pz', 'drop'),
	('a', 'plural'),
	('p', 'drop'), ('z', 'drop'), ('x', 'drop'), ('b', 'drop'),
	('e', 'drop'),
	('n', 'keep'), ('i', 'keep'), ('m', 'keep'),
]

# Types whose upstream spelling is not CamelCase-to-snake friendly or which
# take a dbsql-specific name.
TYPE_OVERRIDES = {
	'sqlite3': 'DBSQL',
	'Parse': 'parser_t',
	'Btree': 'sm_t',
	'BtCursor': 'sm_cursor_t',
	'BtShared': 'sm_shared_t',
	'BtreePayload': 'sm_payload_t',
	'Mem': 'mem_t',
	'Vdbe': 'vdbe_t',
	'VdbeOp': 'vdbe_op_t',
	'VdbeCursor': 'vdbe_cursor_t',
	'u8': 'u_int8_t', 'u16': 'u_int16_t', 'u32': 'u_int32_t',
	'u64': 'u_int64_t', 'i8': 'int8_t', 'i16': 'int16_t', 'i64': 'int64_t',
	'LogEst': 'log_est_t', 'VList': 'vlist_t', 'yDbMask': 'db_mask_t',
	'ynVar': 'yn_var_t', 'tRowcnt': 'rowcnt_t', 'bft': 'bft_t',
	'Pgno': 'pgno_t', 'uptr': 'uptr_t', 'sqlite_uint64': 'u_int64_t',
	'sqlite_int64': 'int64_t', 'sqlite3_uint64': 'u_int64_t',
	'sqlite3_int64': 'int64_t',
}

# Public types -> public dbsql handle names.
API_TYPES = {
	# The connection handle is DBSQL (like libdb's DB, DB_ENV).  Every
	# other public type is a lower-case _t typedef, as in 2007 dbsql
	# (dbsql_stmt_t, dbsql_func_t, dbsql_value_t): DBSQL_BLOB, DBSQL_TEXT,
	# ... are column-type constants and cannot double as type names.
	'sqlite3': 'DBSQL',
	'sqlite3_stmt': 'dbsql_stmt_t',
	'sqlite3_value': 'dbsql_value_t',
	'sqlite3_context': 'dbsql_func_t',
	'sqlite3_str': 'dbsql_str_t',
	'sqlite3_vtab': 'dbsql_vtab_t',
	'sqlite3_vtab_cursor': 'dbsql_vtab_cursor_t',
	'sqlite3_module': 'dbsql_module_t',
	'sqlite3_index_info': 'dbsql_index_info_t',
	'sqlite3_index_constraint': 'dbsql_index_constraint_t',
	'sqlite3_index_constraint_usage': 'dbsql_index_constraint_usage_t',
	'sqlite3_index_orderby': 'dbsql_index_orderby_t',
	'sqlite3_blob': 'dbsql_blob_t',
	'sqlite3_backup': 'dbsql_backup_t',
	'sqlite3_mutex': 'dbsql_mutex_t',
	'sqlite3_mutex_methods': 'dbsql_mutex_methods_t',
	'sqlite3_mem_methods': 'dbsql_mem_methods_t',
	'sqlite3_api_routines': 'dbsql_api_routines_t',
	'sqlite3_file': 'dbsql_file_t',
	'sqlite3_io_methods': 'dbsql_io_methods_t',
	'sqlite3_vfs': 'dbsql_vfs_t',
	'sqlite3_pcache': 'dbsql_pcache_t',
	'sqlite3_pcache_page': 'dbsql_pcache_page_t',
	'sqlite3_pcache_methods': 'dbsql_pcache_methods_t',
	'sqlite3_pcache_methods2': 'dbsql_pcache_methods2_t',
	'sqlite3_snapshot': 'dbsql_snapshot_t',
	'sqlite3_rtree_geometry': 'dbsql_rtree_geometry_t',
	'sqlite3_rtree_query_info': 'dbsql_rtree_query_info_t',
	'sqlite3_rtree_dbl': 'dbsql_rtree_dbl_t',
	'sqlite3_filename': 'dbsql_filename_t',
	'sqlite3_callback': 'dbsql_callback',
	'sqlite3_destructor_type': 'dbsql_destructor_type',
	'sqlite3_syscall_ptr': 'dbsql_syscall_ptr',
	'sqlite3_loadext_entry': 'dbsql_loadext_entry',
	'sqlite3_int64': 'int64_t',
	'sqlite3_uint64': 'u_int64_t',
	'sqlite_int64': 'int64_t',
	'sqlite_uint64': 'u_int64_t',
}

_CAMEL1 = re.compile(r'([A-Z]+)([A-Z][a-z])')
_CAMEL2 = re.compile(r'([a-z0-9])([A-Z])')


def snake(s):
	"""CamelCase / mixedCase -> snake_case (keeps existing underscores)."""
	s = _CAMEL1.sub(r'\1_\2', s)
	s = _CAMEL2.sub(r'\1_\2', s)
	s = re.sub(r'_+', '_', s)
	return s.lower().strip('_') or s.lower()


# Words that are already plural or mass nouns: 'aSortFlags' is an array
# of flags, not of 'flagses'.  (aStatus -> statuses is still right.)
ALREADY_PLURAL = set('''flags args bytes cols ops fields entries terms
values names vars regs pages cells keys items rows idxs data info
stats counts lens sizes ptrs offsets results colls deps hints bits
parts digits chars aliases bufs mems refs nodes ids codes points
funcs idx key_info'''.split())


def plural(w):
	if w in ALREADY_PLURAL:
		return w
	if w.endswith('s') and len(w) > 3 and w[-2] != 's':
		return w		# already plural: windowFuncs, aStats
	if w.endswith(('s', 'x', 'z', 'ch', 'sh')):
		return w + 'es'
	if w.endswith('y') and len(w) > 1 and w[-2] not in 'aeiou':
		return w[:-1] + 'ies'
	return w + 's'


def is_camel(t):
	return re.search(r'[a-z][A-Z]', t) is not None


class Renamer:
	def __init__(self, overrides=None, func_overrides=None, typedefs=(),
		     macros=(), keep=()):
		self.overrides = dict(overrides or {})
		self.func_overrides = dict(func_overrides or {})
		self.typedefs = set(typedefs)	# upstream CamelCase typedefs
		self.macros = set(macros)	# upstream function-like CamelCase macros
		self.keep = set(keep)		# never rename
		self.member_overrides = {}
		self.macro_upper_clash = set()
		self.func_alias_macros = set()
		self.struct_tags = {}		# struct tag -> typedef name
		self.global_overrides = {}	# set by resolve_globals()
		self.reserved = set()		# libc/libdb/system-header names
		self.filescope = set()		# file-scope functions/variables
		self.enums = set()		# enumerator and #define'd constants
		self.cache = {}

	# -- primary/fallback forms for a local/field ----------------------
	def local_forms(self, t):
		"""Return (primary, fallback) readable forms of a Hungarian name."""
		for pfx, how in PREFIX_RULES:
			if t.startswith(pfx) and len(t) > len(pfx) and \
			    t[len(pfx)].isupper():
				rest = snake(t[len(pfx):])
				fb = pfx + '_' + rest
				if how == 'drop':
					return rest, fb
				if how == 'plural':
					parts = rest.split('_')
					parts[-1] = plural(parts[-1])
					return '_'.join(parts), fb
				return fb, pfx + rest.replace('_', '')
		if is_camel(t):
			s = snake(t)
			return s, s + '_'
		return t, t

	def map(self, t, func=None):
		if func is not None:
			k = func + ':' + t
			if k in self.func_overrides:
				return self.func_overrides[k]
			# names.map entries use the bare function name
			bare = func.rsplit('::', 1)[-1] + ':' + t
			if bare in self.func_overrides:
				return self.func_overrides[bare]
		if t in self.cache:
			return self.cache[t]
		r = self._map(t)
		self.cache[t] = r
		return r

	def _map(self, t):
		if t in self.keep or t in C_KEYWORDS:
			return t
		if t in self.overrides:
			return self.overrides[t]
		if t in self.global_overrides:
			return self.global_overrides[t]
		if t in API_TYPES:
			return API_TYPES[t]
		if t in TYPE_OVERRIDES:
			return TYPE_OVERRIDES[t]
		if t.startswith('sqlite3_'):
			return 'dbsql_' + t[len('sqlite3_'):]
		if t.startswith('sqlite_') and KEEP_SQL_NAMES and \
		    t not in ('sqlite_int64', 'sqlite_uint64'):
			return t
		if t.startswith('sqlite_'):
			# SQL function names stringized by DFUNCTION(#name), test
			# hooks (sqlite_search_count), schema names...
			return 'dbsql_' + t[len('sqlite_'):]
		if t.startswith('SQLITE_'):
			return 'DBSQL_' + t[len('SQLITE_'):]
		if t.startswith('sqlite3Btree'):
			return '__sm_' + snake(t[len('sqlite3Btree'):])
		if t.startswith('sqlite3Pager'):
			return '__sm_pager_' + snake(t[len('sqlite3Pager'):])
		if t.startswith('sqlite3Os'):
			return '__dbsql_os_' + snake(t[len('sqlite3Os'):])
		if re.match(r'^sqlite3[A-Z]', t):
			return '__' + snake(t[len('sqlite3'):])
		if t.startswith('sqlite3'):	# sqlite3Config etc. lower-case tail
			return '__dbsql' + t[len('sqlite3'):]
		m = re.match(r'^(Sqlite|Tclsqlite)(\w*)_(Init|SafeInit|Unload|SafeUnload)$', t)
		if m:
			# Tcl package entry points keep Tcl's Pkg_Init shape.
			pre = 'Dbsql' if m.group(1) == 'Sqlite' else 'Tcldbsql'
			return pre + m.group(2) + '_' + m.group(3)
		if re.search(r'sqlite', t, re.I):
			# Anything else spelled with sqlite: header guards
			# (SQLITE3_H, SQLITEINT_H), SQLITE3_TEXT, sqliteHashFirst,
			# INLINEFUNC_sqlite_offset ... replace the word in place,
			# then apply the CamelCase rules to the result.
			r = re.sub(r'SQLITE3?', 'DBSQL', t)
			r = re.sub(r'[Ss]qlite3?', 'dbsql', r)
			if r != t:
				return self._map(r) if re.search(r'[a-z][A-Z]', r) \
				    else r
		if t in self.typedefs:
			return snake(t) + '_t'
		if t in self.struct_tags:
			# 'struct Foo' where Foo is (also) a typedef name maps to
			# the same identifier as the typedef: struct foo_t is
			# then legal C and unambiguous.
			return snake(self.struct_tags[t]) + '_t'
		if t in self.filescope and t not in self.macros and \
		    (is_camel(t) or t[0].isupper()):
			# file-scope (often static) camelCase functions and
			# variables: __snake.  Lower-case file-scope names
			# (main, azCompileOpt...) fall through to the local rules.
			return '__' + snake(t)
		if t in self.macros and t in self.func_alias_macros:
			# '#define fooFunc barFunc' stands in for a function:
			# rename it like one.
			return '__' + snake(t)
		if t in self.macros:
			# Macros keep their spelling when they are already
			# upper-case or PREFIX_ style (OP_FkCheck, TK_*), when
			# they shadow a lower-case C/libc name (memcpy), or when
			# upper-casing would merge two macros (etESCAPE_q/_Q).
			if re.match(r'^[A-Z][A-Z0-9]*_', t) or t.isupper() or \
			    t.islower() or t.upper() in self.macro_upper_clash:
				return t
			return snake(t).upper()
		if t in self.enums:
			return t
		if t[0].isupper() and is_camel(t):
			# CamelCase non-typedef: static functions or struct tags.
			return '__' + snake(t)
		prim, _ = self.local_forms(t)
		if prim in RESERVED:
			_, fb = self.local_forms(t)
			return fb
		return prim

	def map_member(self, t):
		"""Struct/union member names: the plain rule, never a
		per-scope fallback, so a field is spelled the same in every file.
		Collisions between members of one aggregate are checked
		separately (check_members) and fixed with global overrides."""
		if t in self.member_overrides:
			return self.member_overrides[t]
		return self.map(t)

	def is_global(self, t):
		"""True when t maps by a global rule (API, type, macro, internal
		function) rather than the local/field Hungarian rule."""
		return (t in self.overrides or t in API_TYPES or
		    t in TYPE_OVERRIDES or t.startswith(('sqlite3', 'SQLITE_')) or
		    t in self.typedefs or t in self.macros or
		    t in self.enums or t in C_KEYWORDS or
		    (t in self.filescope and (is_camel(t) or t[0].isupper())) or
		    (t[0].isupper() and is_camel(t)))

	def fallback(self, t):
		"""The prefixed form of a Hungarian local (zSql -> z_sql), or None
		when t has no distinct fallback."""
		if t in self.keep:
			return None
		_, fb = self.local_forms(t)
		return fb if fb != self._map(t) else None


def resolve_globals(renamer, names, report):
	"""Make the global (ordinary-identifier, file-scope) namespace
	injective across the whole tree, and keep it clear of libc, libdb and
	system-header names.

	SQLite has both an internal sqlite3FooBar() and a static fooBar() in
	places; both would map to __foo_bar.  The sqlite3* name keeps the
	readable form (it is the cross-file API); a colliding static becomes
	__<file>_foo_bar style via the fallback below.  Any result that is in
	the reserved set (e.g. __error, which FreeBSD's errno macro calls) is
	prefixed with __dbsql_.
	"""
	by_new = {}
	for t in sorted(names):
		by_new.setdefault(renamer.map(t), set()).add(t)
	for new, group in sorted(by_new.items()):
		if new in renamer.reserved and new not in group:
			for t in sorted(group):
				if renamer.is_global(t) and new.startswith('__'):
					fb = '__dbsql_' + new[2:]
					renamer.global_overrides[t] = fb
					report.append('global %s -> %s (reserved %s)' %
					    (t, fb, new))
			continue
		if len(group) < 2:
			continue
		ranked = sorted(group, key=lambda x: (not x.startswith('sqlite3'),
		    not x[0].isupper(), len(x), x))
		owner = ranked[0]
		used = set(renamer.map(x) for x in names)
		for t in ranked[1:]:
			if not new.startswith('__'):
				continue
			if renamer.map(t) != new:
				continue
			# A static that shadows the cross-file sqlite3 name: give it
			# a "_local" suffix (it is file-local by construction).
			fb, n = '__' + new[2:] + '_local', 2
			while fb in used or fb in renamer.reserved:
				fb = '__%s_local%d' % (new[2:], n)
				n += 1
			used.add(fb)
			renamer.global_overrides[t] = fb
			report.append('global %s -> %s (collision with %s on %s)' %
			    (t, fb, owner, new))
	renamer.cache.clear()
