#!/usr/bin/env python3
"""
inventory -- survey identifiers in the upstream sources selected for dbsql.

Prints counts by category so the rename map can be checked for coverage:
  api	sqlite3_* public functions/types
  int	sqlite3Xxx internal functions/vars
  const	SQLITE_* macros
  types	CamelCase typedef/struct names (Table, Expr, ...)
  hung	Hungarian-style locals/fields (pTab, zName, nCol, aOp, iDb, xFunc, bFoo)
"""
import collections, re, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from ctok import tokenize

HUNG = re.compile(r'^(p|z|n|a|i|x|b|ap|az|aa|pz)[A-Z][A-Za-z0-9]*$')
TYPEDEF = re.compile(r'typedef\s+(?:struct|union|enum)\s+(\w+)\s+(\w+)\s*;')

def main(paths):
	cats = collections.defaultdict(collections.Counter)
	types = set()
	for p in paths:
		src = open(p, encoding='utf-8', errors='surrogateescape').read()
		for m in TYPEDEF.finditer(src):
			types.add(m.group(2))
		for kind, t in tokenize(src):
			if kind != 'id':
				continue
			if t.startswith('sqlite3_'):
				cats['api'][t] += 1
			elif re.match(r'^sqlite3[A-Z]', t):
				cats['int'][t] += 1
			elif t.startswith('SQLITE_'):
				cats['const'][t] += 1
			elif HUNG.match(t):
				cats['hung'][t] += 1
	for p in paths:
		src = open(p, encoding='utf-8', errors='surrogateescape').read()
		for kind, t in tokenize(src):
			if kind == 'id' and t in types:
				cats['types'][t] += 1
	for c in ('api', 'int', 'const', 'types', 'hung'):
		print('%-6s distinct=%5d uses=%7d' % (c, len(cats[c]), sum(cats[c].values())))
	if '-v' in os.environ.get('INV', ''):
		for c in cats:
			print(c, cats[c].most_common(40))

if __name__ == '__main__':
	main(sys.argv[1:])
