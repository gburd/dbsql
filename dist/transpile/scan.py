"""
scan -- structural analysis of a token stream for the renamer.

For each file, scan() returns:

  scopes	list of Scope(kind, label, ordinary, members)
		kind is 'func' (a function signature + body), 'agg' (a
		struct/union/enum body), 'macro' (one #define) or 'action' (a
		lemon rule action or directive block).  'ordinary' holds token
		indices in C's ordinary-identifier namespace; 'members' holds
		indices of member names (after '.' or '->', or declared in an
		aggregate body).
  filescope	identifiers declared at file scope (functions and variables)
  macros	names defined by #define
  macro_ids	identifiers appearing in #define bodies
  member_idx	set of token indices that are member references/declarations

The analysis is heuristic (no preprocessing) but conservative: every
identifier token lands in exactly one scope or at file level.
"""

import collections

SIG = ('id', 'num', 'str', 'chr', 'op', 'pp', 'hdr')
AGG_KW = ('struct', 'union', 'enum')
TYPE_KW = set('''auto char const double extern float inline int long register
restrict short signed static unsigned void volatile _Bool bool typedef
SQLITE_PRIVATE SQLITE_API SQLITE_NOINLINE SQLITE_INLINE SQLITE_EXTERN
SQLITE_CDECL SQLITE_APICALL SQLITE_STDCALL SQLITE_SYSAPI SQLITE_WSD
SQLITE_DEPRECATED SQLITE_EXPERIMENTAL SQLITE_NOINLINE'''.split())

Scope = collections.namedtuple('Scope', 'kind label ordinary members')


def _prev_sig(toks, i):
	j = i - 1
	while j >= 0 and toks[j][0] not in SIG:
		j -= 1
	return j


def _next_sig(toks, i):
	j = i + 1
	while j < len(toks) and toks[j][0] not in SIG:
		j += 1
	return j


def scan(toks, yacc=False):
	n = len(toks)
	scopes = []
	filescope = set()
	macros = set()
	macro_ids = set()
	member_idx = set()

	# --- preprocessor lines: one 'macro' scope per #define --------------
	pp_lines = []	# (start, end) token ranges of directive lines
	i = 0
	while i < n:
		if toks[i][0] == 'pp':
			j = i
			while j < n and toks[j][0] != 'nl':
				j += 1
			pp_lines.append((i, j))
			i = j
		i += 1
	in_pp = bytearray(n)
	for s, e in pp_lines:
		for k in range(s, e):
			in_pp[k] = 1
		ids = [k for k in range(s, e) if toks[k][0] == 'id']
		if len(ids) >= 2 and toks[ids[0]][1] == 'define':
			name = toks[ids[1]][1]
			macros.add(name)
			body = ids[2:]
			for k in body:
				macro_ids.add(toks[k][1])
			scopes.append(Scope('macro', '#' + name, body, []))

	# --- member references (after '.' or '->') --------------------------
	for k in range(n):
		if toks[k][0] == 'id':
			p = _prev_sig(toks, k)
			if p >= 0 and toks[p][0] == 'op' and toks[p][1] in ('.', '->'):
				member_idx.add(k)

	# --- brace/paren structure outside the preprocessor -----------------
	depth = 0		# effective brace depth (transparent braces excluded)
	pdepth = 0		# paren depth at brace depth 0
	stack = []		# brace kinds
	pending = []		# id indices at depth 0 since last boundary
	cur = None		# (kind, label, ids) of the open depth-0 scope
	stmt_first = None	# first significant token of the depth-0 statement
	for k in range(n):
		if in_pp[k]:
			continue
		kind, t = toks[k]
		if kind not in SIG:
			continue
		if depth == 0 and stmt_first is None:
			stmt_first = k
		if kind == 'op' and t == '{':
			p = _prev_sig(toks, k)
			pk, pt = toks[p] if p >= 0 else ('', '')
			if depth == 0:
				bkind, label = _classify(toks, k, p, pending, yacc)
				if bkind == 'transparent':
					stack.append('transparent')
					pending = []
					stmt_first = None
					continue
				cur = [bkind, label, list(pending) if bkind == 'func'
				    else []]
				if bkind == 'agg':
					cur[2] = []
			stack.append('real')
			depth += 1
			continue
		if kind == 'op' and t == '}':
			if stack and stack[-1] == 'transparent':
				stack.pop()
				continue
			if stack:
				stack.pop()
			depth -= 1
			if depth == 0 and cur is not None:
				bk, label, ids = cur
				if bk == 'agg':
					scopes.append(Scope('agg', label,
					    [x for x in ids if x not in member_idx],
					    _agg_members(toks, ids)))
				elif bk in ('func', 'action'):
					scopes.append(Scope(bk, label,
					    [x for x in ids if x not in member_idx],
					    []))
				cur = None
				if bk in ('func', 'action'):
					pending = []
					stmt_first = None
			continue
		if depth > 0:
			if kind == 'id' and cur is not None:
				cur[2].append(k)
			continue
		# depth 0
		if kind == 'op' and t == '(':
			pdepth += 1
		elif kind == 'op' and t == ')':
			pdepth -= 1
		elif kind == 'op' and t == ';':
			_filescope_stmt(toks, pending, filescope, stmt_first)
			proto = _proto_params(toks, pending)
			if proto:
				scopes.append(Scope('proto', None, proto, []))
			pending = []
			stmt_first = None
			pdepth = 0
			continue
		if kind == 'id':
			pending.append(k)
	# function definitions: their names are file-scope declarations too
	for sc in scopes:
		if sc.kind == 'func' and sc.label:
			filescope.add(sc.label)
	filescope -= TYPE_KW
	filescope -= set(AGG_KW)
	return scopes, filescope, macros, macro_ids, member_idx


def _classify(toks, k, p, pending, yacc):
	"""Classify a depth-0 '{' at index k (previous significant token p)."""
	pk, pt = toks[p] if p >= 0 else ('', '')
	if pk == 'str':			# extern "C" {
		return 'transparent', None
	if yacc:
		# %include { ... } / %code { ... } hold ordinary C: transparent.
		q = _prev_sig(toks, p)
		if pk == 'id' and q >= 0 and toks[q] == ('op', '%') and \
		    pt in ('include', 'code'):
			return 'transparent', None
		if pk == 'op' and pt == '.':
			return 'action', None
		if q >= 0 and toks[q] == ('op', '%') or (pk == 'id' and
		    _directive_before(toks, p)):
			return 'action', None
	if pk == 'op' and pt == ')':
		return 'func', _func_name(toks, pending)
	if pk == 'id' and pt in AGG_KW:
		return 'agg', None
	if pk == 'id':
		q = _prev_sig(toks, p)
		if q >= 0 and toks[q][0] == 'id' and toks[q][1] in AGG_KW:
			return 'agg', pt
	if pk == 'op' and pt in ('=', ',', '{', '('):
		return 'init', None
	return 'init', None


def _directive_before(toks, p):
	q = p
	for _ in range(4):
		q = _prev_sig(toks, q)
		if q < 0:
			return False
		if toks[q] == ('op', '%'):
			return True
		if toks[q][0] == 'op' and toks[q][1] in ('.', '}', ';'):
			return False
	return False


def _func_name(toks, pending):
	"""Name of the function whose signature is in 'pending': the last
	identifier at paren depth 0 that is followed by '('."""
	name = None
	depth = 0
	for k in pending:
		pass
	# walk tokens from first pending to last, tracking parens
	if not pending:
		return None
	start, end = pending[0], pending[-1]
	for j in range(start, end + 1):
		kind, t = toks[j]
		if kind == 'op' and t == '(':
			depth += 1
		elif kind == 'op' and t == ')':
			depth -= 1
		elif kind == 'id' and depth == 0:
			nx = _next_sig(toks, j)
			if nx < len(toks) and toks[nx] == ('op', '('):
				name = t
	return name


def _filescope_stmt(toks, pending, filescope, stmt_first):
	"""Record identifiers declared by a depth-0 statement ending in ';':
	function prototypes and variable declarations (not typedefs).  Only
	tokens outside every (), [] and {} count: members of an inline
	'static struct { ... } aFoo[]' body are not file-scope names."""
	if not pending:
		return
	if stmt_first is not None and toks[stmt_first] == ('id', 'typedef'):
		return
	depth = 0
	start, end = pending[0], pending[-1]
	for j in range(start, end + 1):
		kind, t = toks[j]
		if kind == 'op' and t in ('(', '[', '{'):
			depth += 1
		elif kind == 'op' and t in (')', ']', '}'):
			depth -= 1
		elif kind == 'id' and depth == 0 and t not in TYPE_KW and \
		    t not in AGG_KW:
			nx = _next_sig(toks, j)
			pv = _prev_sig(toks, j)
			if nx >= len(toks):
				continue
			nk, nt = toks[nx]
			if pv >= 0 and toks[pv][0] == 'id' and \
			    toks[pv][1] in AGG_KW:
				continue
			if nk == 'op' and nt in ('(', '[', '=', ';', ','):
				filescope.add(t)


def _agg_members(toks, ids):
	"""Member declarator names inside an aggregate body: in each
	';'-terminated declaration (at the body's own brace level), the
	identifiers that are directly followed by ';', ',', '[', ':', '=' or by
	')' + '(' (a function-pointer member '(*xFoo)(...)').  Nested aggregate
	bodies contribute their own members too (anonymous unions)."""
	out = []
	for k in ids:
		nx = _next_sig(toks, k)
		if nx >= len(toks):
			continue
		nk, nt = toks[nx]
		pv = _prev_sig(toks, k)
		pk, pt = toks[pv] if pv >= 0 else ('', '')
		if pk == 'id' and pt in AGG_KW:
			continue		# struct tag
		if pk == 'op' and pt in ('.', '->'):
			continue
		if nk == 'op' and nt in (';', ',', '[', ':', '='):
			out.append(k)
		elif nk == 'op' and nt == ')' and pk == 'op' and pt == '*':
			nn = _next_sig(toks, nx)
			if nn < len(toks) and toks[nn] == ('op', '('):
				out.append(k)
	return out


def _proto_params(toks, pending):
	"""Identifiers inside the parentheses of a depth-0 declaration
	(prototype parameter names, and parameter types).  Treated as one
	scope so two parameters cannot be renamed to the same name."""
	if not pending:
		return []
	out = []
	depth = 0
	start, end = pending[0], pending[-1]
	for j in range(start, end + 1):
		kind, t = toks[j]
		if kind == 'op' and t == '(':
			depth += 1
		elif kind == 'op' and t == ')':
			depth -= 1
		elif kind == 'id' and depth > 0:
			out.append(j)
	return out


def nested_aggs(toks):
	"""Member declarators of every struct/union body at ANY depth (local
	structs inside functions, anonymous unions): list of index lists."""
	out = []
	n = len(toks)
	stack = []	# (is_agg, ids)
	for k in range(n):
		kind, t = toks[k]
		if kind == 'op' and t == '{':
			p = _prev_sig(toks, k)
			is_agg = False
			if p >= 0:
				pk, pt = toks[p]
				if pk == 'id' and pt in ('struct', 'union'):
					is_agg = True
				elif pk == 'id':
					q = _prev_sig(toks, p)
					if q >= 0 and toks[q][0] == 'id' and \
					    toks[q][1] in ('struct', 'union'):
						is_agg = True
			stack.append((is_agg, []))
		elif kind == 'op' and t == '}':
			if stack:
				is_agg, ids = stack.pop()
				if is_agg:
					out.append(_agg_members(toks, ids))
		elif kind == 'id' and stack:
			stack[-1][1].append(k)
	return out
