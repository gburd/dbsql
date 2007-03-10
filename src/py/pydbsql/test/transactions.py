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
# Copyright (C) 2005 Gerhard Häring <gh@ghaering.de>
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

# Tests transactions

import os, unittest
import pydbsql.dbapi2 as dbsql

def get_db_path():
    return "dbsql_testdb"

class TransactionTests(unittest.TestCase):
    def setUp(self):
        try:
            os.remove(get_db_path())
        except:
            pass

        self.con1 = dbsql.connect(get_db_path(), timeout=0.1)
        self.cur1 = self.con1.cursor()

        self.con2 = dbsql.connect(get_db_path(), timeout=0.1)
        self.cur2 = self.con2.cursor()

    def tearDown(self):
        self.cur1.close()
        self.con1.close()

        self.cur2.close()
        self.con2.close()

        os.unlink(get_db_path())

    def CheckDMLdoesAutoCommitBefore(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.cur1.execute("create table test2(j)")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 1)

    def CheckInsertStartsTransaction(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 0)

    def CheckUpdateStartsTransaction(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.con1.commit()
        self.cur1.execute("update test set i=6")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchone()[0]
        self.failUnlessEqual(res, 5)

    def CheckDeleteStartsTransaction(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.con1.commit()
        self.cur1.execute("delete from test")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 1)

    def CheckReplaceStartsTransaction(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.con1.commit()
        self.cur1.execute("replace into test(i) values (6)")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 1)
        self.failUnlessEqual(res[0][0], 5)

    def CheckToggleAutoCommit(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        self.con1.isolation_level = None
        self.failUnlessEqual(self.con1.isolation_level, None)
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 1)

        self.con1.isolation_level = "DEFERRED"
        self.failUnlessEqual(self.con1.isolation_level , "DEFERRED")
        self.cur1.execute("insert into test(i) values (5)")
        self.cur2.execute("select i from test")
        res = self.cur2.fetchall()
        self.failUnlessEqual(len(res), 1)

    def CheckRaiseTimeout(self):
        self.cur1.execute("create table test(i)")
        self.cur1.execute("insert into test(i) values (5)")
        try:
            self.cur2.execute("insert into test(i) values (5)")
            self.fail("should have raised an OperationalError")
        except dbsql.OperationalError:
            pass
        except:
            self.fail("should have raised an OperationalError")

class SpecialCommandTests(unittest.TestCase):
    def setUp(self):
        self.con = dbsql.connect(":memory:")
        self.cur = self.con.cursor()

    def CheckVacuum(self):
        self.cur.execute("create table test(i)")
        self.cur.execute("insert into test(i) values (5)")
        self.cur.execute("vacuum")

    def CheckDropTable(self):
        self.cur.execute("create table test(i)")
        self.cur.execute("insert into test(i) values (5)")
        self.cur.execute("drop table test")

    def CheckPragma(self):
        self.cur.execute("create table test(i)")
        self.cur.execute("insert into test(i) values (5)")
        self.cur.execute("pragma count_changes=1")

    def tearDown(self):
        self.cur.close()
        self.con.close()

def suite():
    default_suite = unittest.makeSuite(TransactionTests, "Check")
    special_command_suite = unittest.makeSuite(SpecialCommandTests, "Check")
    return unittest.TestSuite((default_suite, special_command_suite))

def test():
    runner = unittest.TextTestRunner()
    runner.run(suite())

if __name__ == "__main__":
    test()
