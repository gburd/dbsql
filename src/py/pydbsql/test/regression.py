#-*- coding: ISO-8859-1 -*-
#
# DBSQL - A SQL database engine.
#
# Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# There are special exceptions to the terms and conditions of the GPL as it
# is applied to this software. View the full text of the exception in file
# LICENSE_EXCEPTIONS in the directory of this software distribution.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# http://creativecommons.org/licenses/GPL/2.0/
#
# Copyright (C) 2006 Gerhard Häring <gh@ghaering.de>
#
# This software is provided 'as-is', without any express or implied
# warranty.  In no event will the authors be held liable for any damages
# arising from the use of this software.
#
# Permission is granted to anyone to use this software for any purpose,
# including commercial applications, and to alter it and redistribute it
# freely, subject to the following restrictions:
#
# 1. The origin of this software must not be misrepresented; you must not
#    claim that you wrote the original software. If you use this software
#    in a product, an acknowledgment in the product documentation would be
#    appreciated but is not required.
# 2. Altered source versions must be plainly marked as such, and must not be
#    misrepresented as being the original software.
# 3. This notice may not be removed or altered from any source distribution.

# pydbsql regression tests

import unittest
import pydbsql.dbapi2 as dbsql

class RegressionTests(unittest.TestCase):
    def setUp(self):
        self.con = dbsql.connect(":memory:")

    def tearDown(self):
        self.con.close()

    def CheckPragmaUserVersion(self):
        # This used to crash pydbsql because this pragma command returns NULL for the column name
        cur = self.con.cursor()
        cur.execute("pragma user_version")

    def CheckPragmaSchemaVersion(self):
        # This still crashed pydbsql <= 2.2.1
        con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_COLNAMES)
        try:
            cur = self.con.cursor()
            cur.execute("pragma schema_version")
        finally:
            cur.close()
            con.close()

    def CheckStatementReset(self):
        # pydbsql 2.1.0 to 2.2.0 have the problem that not all statements are
        # reset before a rollback, but only those that are still in the
        # statement cache. The others are not accessible from the connection object.
        con = dbsql.connect(":memory:", cached_statements=5)
        cursors = [con.cursor() for x in xrange(5)]
        cursors[0].execute("create table test(x)")
        for i in range(10):
            cursors[0].executemany("insert into test(x) values (?)", [(x,) for x in xrange(10)])

        for i in range(5):
            cursors[i].execute(" " * i + "select x from test")

        con.rollback()

    def CheckColumnNameWithSpaces(self):
        cur = self.con.cursor()
        cur.execute('select 1 as "foo bar [datetime]"')
        self.failUnlessEqual(cur.description[0][0], "foo bar")

        cur.execute('select 1 as "foo baz"')
        self.failUnlessEqual(cur.description[0][0], "foo baz")

    def CheckStatementAvailable(self):
        # pydbsql up to 2.3.2 crashed on this, because the active statement handle was not checked
        # before trying to fetch data from it. close() destroys the active statement ...
        con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_DECLTYPES)
        cur = con.cursor()
        cur.execute("select 4 union select 5")
        cur.close()
        cur.fetchone()
        cur.fetchone()

def suite():
    regression_suite = unittest.makeSuite(RegressionTests, "Check")
    return unittest.TestSuite((regression_suite,))

def test():
    runner = unittest.TextTestRunner()
    runner.run(suite())

if __name__ == "__main__":
    test()
