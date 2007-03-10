#!/bin/sh -
#
# $Id: chk.inc 7 2007-02-03 13:34:17Z gburd $
#
# Check for inclusion of dbsql_config.h after "const" or other includes.

d=../..

# Test must be run from the top-level directory, not from a test directory.
[ -f $d/LICENSE ] || {
	echo 'FAIL: cannot find source distribution directory.'
	exit 1
}

t1=__1
t2=__2

(cd $d && find . -name '*.[chys]' -o -name '*.cpp' |
    xargs egrep -l '#include.*dbsql_config.h') | tee /tmp/o > $t1

(for i in `cat $t1`; do
	egrep -w 'dbsql_config.h|const' /dev/null $d/$i | head -1
done) > $t2

if egrep const $t2 > /dev/null; then
	echo 'FAIL: found const before include of dbsql_config.h'
	egrep const $t2
	exit 1
fi

:> $t2
for i in `cat $t1`; do
	egrep -w '#include' /dev/null $d/$i | head -1 >> $t2
done

if egrep -v dbsql_config.h $t2 > /dev/null; then
	echo 'FAIL: found includes before include of dbsql_config.h'
	egrep -v dbsql_config.h $t2
	exit 1
fi

exit 0
