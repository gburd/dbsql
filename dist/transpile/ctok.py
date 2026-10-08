"""
ctok -- a lossless C tokenizer.

tokenize(text) returns a list of (kind, text) pairs whose texts concatenate
back to the input exactly.  Kinds:

	ws	whitespace (including backslash-newline continuations)
	nl	a newline
	cmt	a /* */ or // comment
	id	an identifier or keyword
	num	a preprocessing number
	str	a string literal (with any encoding prefix)
	chr	a character literal
	pp	a '#' that begins a preprocessor directive
	hdr	a <header> name following #include
	op	any other punctuator

The tokenizer is deliberately permissive: it never fails, so unusual input
(stray characters, unterminated literals) is passed through unchanged.
"""

import re

_TOKEN = re.compile(r"""
	(?P<nl>\n)
	|(?P<ws>(?:[ \t\r\f\v]|\\\n)+)
	|(?P<cmt>/\*.*?\*/|//(?:[^\n\\]|\\\n|\\.)*)
	|(?P<str>(?:u8|[uUL])?"(?:[^"\\\n]|\\.|\\\n)*"?)
	|(?P<chr>(?:u8|[uUL])?'(?:[^'\\\n]|\\.|\\\n)*'?)
	|(?P<id>[A-Za-z_$][A-Za-z0-9_$]*)
	|(?P<num>\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.'])*)
	|(?P<op>\#\#|\#|->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||
	   [-+*/%&|^]=|\.\.\.|.)
""", re.S | re.X)

_HDR = re.compile(r'<[^>\n]*>')


def tokenize(text):
	toks = []
	pos = 0
	line_start = True	# only whitespace/comments seen since last newline
	in_include = False	# just saw "# include"
	pp_words = 0		# tokens seen since a directive '#'
	n = len(text)
	while pos < n:
		if in_include:
			m = _HDR.match(text, pos)
			if m:
				toks.append(('hdr', m.group()))
				pos = m.end()
				in_include = False
				continue
		m = _TOKEN.match(text, pos)
		kind = m.lastgroup
		tok = m.group()
		pos = m.end()
		if kind == 'op' and tok == '#' and line_start:
			kind = 'pp'
			pp_words = 0
		if kind == 'nl':
			line_start = True
			in_include = False
			pp_words = -1
		elif kind in ('ws', 'cmt'):
			pass
		else:
			if kind == 'id' and pp_words == 0 and \
			    tok in ('include', 'include_next', 'import'):
				in_include = True
			elif kind != 'pp':
				in_include = False
			if kind != 'pp' and pp_words >= 0:
				pp_words += 1
			line_start = False
		toks.append((kind, tok))
	return toks


def untokenize(toks):
	return ''.join(t for _, t in toks)


if __name__ == '__main__':
	import sys
	for path in sys.argv[1:]:
		with open(path, encoding='utf-8', errors='surrogateescape') as f:
			src = f.read()
		out = untokenize(tokenize(src))
		assert out == src, path
	print('round-trip ok:', len(sys.argv) - 1, 'files')
