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
# Copyright (C) 2005 Gerhard H�ring <gh@ghaering.de>
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

# Tests for type conversion and detection

import bz2, datetime
import unittest
import pydbsql.dbapi2 as dbsql

class DbsqlTypeTests(unittest.TestCase):
    def setUp(self):
        self.con = dbsql.connect(":memory:")
        self.cur = self.con.cursor()
        self.cur.execute("create table test(i integer, s varchar, f number, b blob)")

    def tearDown(self):
        self.cur.close()
        self.con.close()

    def CheckString(self):
        self.cur.execute("insert into test(s) values (?)", (u"�sterreich",))
        self.cur.execute("select s from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], u"�sterreich")

    def CheckSmallInt(self):
        self.cur.execute("insert into test(i) values (?)", (42,))
        self.cur.execute("select i from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], 42)

    def CheckLargeInt(self):
        num = 2**40
        self.cur.execute("insert into test(i) values (?)", (num,))
        self.cur.execute("select i from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], num)

    def CheckFloat(self):
        val = 3.14
        self.cur.execute("insert into test(f) values (?)", (val,))
        self.cur.execute("select f from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], val)

    def CheckBlob(self):
        val = buffer("Guglhupf")
        self.cur.execute("insert into test(b) values (?)", (val,))
        self.cur.execute("select b from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], val)

    def CheckUnicodeExecute(self):
        self.cur.execute(u"select '�sterreich'")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], u"�sterreich")

class DeclTypesTests(unittest.TestCase):
    class Foo:
        def __init__(self, _val):
            self.val = _val

        def __cmp__(self, other):
            if not isinstance(other, DeclTypesTests.Foo):
                raise ValueError
            if self.val == other.val:
                return 0
            else:
                return 1

        def __conform__(self, protocol):
            if protocol is dbsql.PrepareProtocol:
                return self.val
            else:
                return None

        def __str__(self):
            return "<%s>" % self.val

    def setUp(self):
        self.con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_DECLTYPES)
        self.cur = self.con.cursor()
        self.cur.execute("create table test(i int, s str, f float, b bool, u unicode, foo foo, bin blob)")

        # override float, make them always return the same number
        dbsql.converters["FLOAT"] = lambda x: 47.2

        # and implement two custom ones
        dbsql.converters["BOOL"] = lambda x: bool(int(x))
        dbsql.converters["FOO"] = DeclTypesTests.Foo
        dbsql.converters["WRONG"] = lambda x: "WRONG"

    def tearDown(self):
        del dbsql.converters["FLOAT"]
        del dbsql.converters["BOOL"]
        del dbsql.converters["FOO"]
        self.cur.close()
        self.con.close()

    def CheckString(self):
        # default
        self.cur.execute("insert into test(s) values (?)", ("foo",))
        self.cur.execute('select s as "s [WRONG]" from test')
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], "foo")

    def CheckSmallInt(self):
        # default
        self.cur.execute("insert into test(i) values (?)", (42,))
        self.cur.execute("select i from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], 42)

    def CheckLargeInt(self):
        # default
        num = 2**40
        self.cur.execute("insert into test(i) values (?)", (num,))
        self.cur.execute("select i from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], num)

    def CheckFloat(self):
        # custom
        val = 3.14
        self.cur.execute("insert into test(f) values (?)", (val,))
        self.cur.execute("select f from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], 47.2)

    def CheckBool(self):
        # custom
        self.cur.execute("insert into test(b) values (?)", (False,))
        self.cur.execute("select b from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], False)

        self.cur.execute("delete from test")
        self.cur.execute("insert into test(b) values (?)", (True,))
        self.cur.execute("select b from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], True)

    def CheckUnicode(self):
        # default
        val = u"\xd6sterreich"
        self.cur.execute("insert into test(u) values (?)", (val,))
        self.cur.execute("select u from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], val)

    def CheckFoo(self):
        val = DeclTypesTests.Foo("bla")
        self.cur.execute("insert into test(foo) values (?)", (val,))
        self.cur.execute("select foo from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], val)

    def CheckUnsupportedSeq(self):
        class Bar: pass
        val = Bar()
        try:
            self.cur.execute("insert into test(f) values (?)", (val,))
            self.fail("should have raised an InterfaceError")
        except dbsql.InterfaceError:
            pass
        except:
            self.fail("should have raised an InterfaceError")

    def CheckUnsupportedDict(self):
        class Bar: pass
        val = Bar()
        try:
            self.cur.execute("insert into test(f) values (:val)", {"val": val})
            self.fail("should have raised an InterfaceError")
        except dbsql.InterfaceError:
            pass
        except:
            self.fail("should have raised an InterfaceError")

    def CheckBlob(self):
        # default
        val = buffer("Guglhupf")
        self.cur.execute("insert into test(bin) values (?)", (val,))
        self.cur.execute("select bin from test")
        row = self.cur.fetchone()
        self.failUnlessEqual(row[0], val)

class ColNamesTests(unittest.TestCase):
    def setUp(self):
        self.con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_COLNAMES)
        self.cur = self.con.cursor()
        self.cur.execute("create table test(x foo)")

        dbsql.converters["FOO"] = lambda x: "[%s]" % x
        dbsql.converters["BAR"] = lambda x: "<%s>" % x
        dbsql.converters["EXC"] = lambda x: 5/0
        dbsql.converters["B1B1"] = lambda x: "MARKER"

    def tearDown(self):
        del dbsql.converters["FOO"]
        del dbsql.converters["BAR"]
        del dbsql.converters["EXC"]
        del dbsql.converters["B1B1"]
        self.cur.close()
        self.con.close()

    def CheckDeclTypeNotUsed(self):
        """
        Assures that the declared type is not used when PARSE_DECLTYPES
        is not set.
        """
        self.cur.execute("insert into test(x) values (?)", ("xxx",))
        self.cur.execute("select x from test")
        val = self.cur.fetchone()[0]
        self.failUnlessEqual(val, "xxx")

    def CheckNone(self):
        self.cur.execute("insert into test(x) values (?)", (None,))
        self.cur.execute("select x from test")
        val = self.cur.fetchone()[0]
        self.failUnlessEqual(val, None)

    def CheckColName(self):
        self.cur.execute("insert into test(x) values (?)", ("xxx",))
        self.cur.execute('select x as "x [bar]" from test')
        val = self.cur.fetchone()[0]
        self.failUnlessEqual(val, "<xxx>")

        # Check if the stripping of colnames works. Everything after the first
        # whitespace should be stripped.
        self.failUnlessEqual(self.cur.description[0][0], "x")

    def CheckCaseInConverterName(self):
        self.cur.execute("""select 'other' as "x [b1b1]\"""")
        val = self.cur.fetchone()[0]
        self.failUnlessEqual(val, "MARKER")

    def CheckCursorDescriptionNoRow(self):
        """
        cursor.description should at least provide the column name(s), even if
        no row returned.
        """
        self.cur.execute("select * from test where 0 = 1")
        self.assert_(self.cur.description[0][0] == "x")

class ObjectAdaptationTests(unittest.TestCase):
    def cast(obj):
        return float(obj)
    cast = staticmethod(cast)

    def setUp(self):
        self.con = dbsql.connect(":memory:")
        try:
            del dbsql.adapters[int]
        except:
            pass
        dbsql.register_adapter(int, ObjectAdaptationTests.cast)
        self.cur = self.con.cursor()

    def tearDown(self):
        del dbsql.adapters[(int, dbsql.PrepareProtocol)]
        self.cur.close()
        self.con.close()

    def CheckCasterIsUsed(self):
        self.cur.execute("select ?", (4,))
        val = self.cur.fetchone()[0]
        self.failUnlessEqual(type(val), float)

class BinaryConverterTests(unittest.TestCase):
    def convert(s):
        return bz2.decompress(s)
    convert = staticmethod(convert)

    def setUp(self):
        self.con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_COLNAMES)
        dbsql.register_converter("bin", BinaryConverterTests.convert)

    def tearDown(self):
        self.con.close()

    def CheckBinaryInputForConverter(self):
        testdata = "abcdefg" * 10
        result = self.con.execute('select ? as "x [bin]"', (buffer(bz2.compress(testdata)),)).fetchone()[0]
        self.failUnlessEqual(testdata, result)

class DateTimeTests(unittest.TestCase):
    def setUp(self):
        self.con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_DECLTYPES)
        self.cur = self.con.cursor()
        self.cur.execute("create table test(d date, ts timestamp)")

    def tearDown(self):
        self.cur.close()
        self.con.close()

    def CheckDbsqlDate(self):
        d = dbsql.Date(2004, 2, 14)
        self.cur.execute("insert into test(d) values (?)", (d,))
        self.cur.execute("select d from test")
        d2 = self.cur.fetchone()[0]
        self.failUnlessEqual(d, d2)

    def CheckDbsqlTimestamp(self):
        ts = dbsql.Timestamp(2004, 2, 14, 7, 15, 0)
        self.cur.execute("insert into test(ts) values (?)", (ts,))
        self.cur.execute("select ts from test")
        ts2 = self.cur.fetchone()[0]
        self.failUnlessEqual(ts, ts2)

    def CheckSqlTimestamp(self):
        # The date functions are only available in Dbsql version 3.1 or later
        if dbsql.dbsql_version_info < (3, 1):
            return

        # Dbsql's current_timestamp uses UTC time, while datetime.datetime.now() uses local time.
        now = datetime.datetime.now()
        self.cur.execute("insert into test(ts) values (current_timestamp)")
        self.cur.execute("select ts from test")
        ts = self.cur.fetchone()[0]
        self.failUnlessEqual(type(ts), datetime.datetime)
        self.failUnlessEqual(ts.year, now.year)

    def CheckDateTimeSubSeconds(self):
        ts = dbsql.Timestamp(2004, 2, 14, 7, 15, 0, 500000)
        self.cur.execute("insert into test(ts) values (?)", (ts,))
        self.cur.execute("select ts from test")
        ts2 = self.cur.fetchone()[0]
        self.failUnlessEqual(ts, ts2)

    def CheckDateTimeSubSecondsFloatingPoint(self):
        ts = dbsql.Timestamp(2004, 2, 14, 7, 15, 0, 510241)
        self.cur.execute("insert into test(ts) values (?)", (ts,))
        self.cur.execute("select ts from test")
        ts2 = self.cur.fetchone()[0]
        self.failUnlessEqual(ts, ts2)

def suite():
    dbsql_type_suite = unittest.makeSuite(DbsqlTypeTests, "Check")
    decltypes_type_suite = unittest.makeSuite(DeclTypesTests, "Check")
    colnames_type_suite = unittest.makeSuite(ColNamesTests, "Check")
    adaptation_suite = unittest.makeSuite(ObjectAdaptationTests, "Check")
    bin_suite = unittest.makeSuite(BinaryConverterTests, "Check")
    date_suite = unittest.makeSuite(DateTimeTests, "Check")
    return unittest.TestSuite((dbsql_type_suite, decltypes_type_suite, colnames_type_suite, adaptation_suite, bin_suite, date_suite))

def test():
    runner = unittest.TextTestRunner()
    runner.run(suite())

if __name__ == "__main__":
    test()
