#!/usr/bin/env python3
"""
check-lengths -- flag string literals whose length the rename changed and
that appear on a line with a small integer literal (a likely length or
offset tied to the string: memcpy(x, "dbsql_", 8), z += 7, ...).

usage: check-lengths.py UPSTREAM_DIR OUT_DIR manifest
Prints suspicious lines (upstream line + transformed line).  A suspicious
line that is already covered by a fixup (the numbers changed too) is not
reported.  Exit status 1 when anything is reported.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctok import tokenize

def strings(line):
	return [t for k, t in tokenize(line) if k == 'str']

def nums(line):
	return [t for k, t in tokenize(line) if k == 'num']

def main(up, out, manifest):
	bad = 0
	for l in open(manifest):
		l = l.split('#', 1)[0].split()
		if len(l) < 2 or (len(l) > 2 and l[2] == 'native'):
			continue
		u, d = os.path.join(up, l[0]), os.path.join(out, l[1])
		if not os.path.exists(u) or not os.path.exists(d):
			continue
		ul = open(u, errors='surrogateescape').read().split('\n')
		dl = open(d, errors='surrogateescape').read().split('\n')
		if len(ul) != len(dl):
			continue
		for i, (a, b) in enumerate(zip(ul, dl)):
			# Comment lines: a quoted word there is prose, not code.
			# A comment that quotes a renamed literal next to code with a
			# number (zName[16]; /* skip "sqlite_altertab_" */) is code.
			if re.match(r'\s*(\*\*|\*|/\*|//)', a):
				continue
			sa, sb = strings(a), strings(b)
			if not sa:
				# The literal may only be named in a trailing comment.
				ca = re.findall(r'"(sqlite[A-Za-z_0-9]*)"', a)
				cb = re.findall(r'"(dbsql[A-Za-z_0-9]*)"', b)
				if ca and len(ca) == len(cb) and nums(a) == nums(b) and \
				    any(len(x) != len(y) for x, y in zip(ca, cb)) and \
				    any(re.fullmatch(r'\d+', n) and
				    int(n) in {len(x), len(x) + 1} for n in nums(a)
				    for x in ca):
					print('%s:%d:\n  up:  %s\n  out: %s' % (l[1],
					    i + 1, a.strip(), b.strip()))
					bad += 1
				continue
			if len(sa) != len(sb) or not sa:
				continue
			if all(len(x) == len(y) for x, y in zip(sa, sb)):
				continue
			na = [n for n in nums(a) if re.fullmatch(r'\d+', n) and
			    1 < int(n) < 64]
			if not na:
				continue
			if nums(a) != nums(b):
				continue	# a fixup already adjusted the numbers
			# literals whose length is unrelated to the numbers
			lens = set()
			for x, y in zip(sa, sb):
				if len(x) != len(y):
					lens.update({len(x) - 2, len(x) - 3})
			if not any(int(n) in lens or int(n) in (7, 8, 17, 18) for n in na):
				continue
			print('%s:%d:\n  up:  %s\n  out: %s' % (l[1], i + 1,
			    a.strip(), b.strip()))
			bad += 1
	print('suspicious length/offset sites:', bad)
	return 1 if bad else 0

if __name__ == '__main__':
	sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
