#!/bin/sh
# check-names.sh OUTDIR -- the "no sqlite in code" gate.  Reports every
# occurrence of sqlite (any case) outside comments in transformed sources.
# Comments may keep attribution ("derived from SQLite").
python3.11 - "$1" <<'PY'
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])) if False else '.', ''))
sys.path.insert(0, os.path.expanduser('~/w/tp'))
from ctok import tokenize
root = sys.argv[1]
bad = 0
for d, _, fs in os.walk(root):
	for f in fs:
		if not f.endswith(('.c', '.h', '.y', '.in')):
			continue
		p = os.path.join(d, f)
		src = open(p, encoding='utf-8', errors='surrogateescape').read()
		line = 1
		for k, t in tokenize(src):
			if k != 'cmt' and re.search(r'sqlite', t, re.I):
				print('%s:%d: %s' % (p, line, t[:80]))
				bad += 1
			line += t.count('\n')
print('sqlite occurrences outside comments:', bad)
sys.exit(1 if bad else 0)
PY
