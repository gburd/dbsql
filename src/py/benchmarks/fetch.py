# DBSQL - A SQL database engine.
#
# Copyright (C) 2007-2008  The DBSQL Group, Inc.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#

import time

def yesno(question):
    val = raw_input(question + " ")
    return val.startswith("y") or val.startswith("Y")

use_dbsql = yesno("Use pydbsql?")
if use_dbsql:
    use_custom_types = yesno("Use custom types?")
    use_dictcursor = yesno("Use dict cursor?")
    use_rowcursor = yesno("Use row cursor?")
else:
    use_tuple = yesno("Use rowclass=tuple?")

if use_dbsql:
    from dbsql import dbapi2 as dbsql
else:
    import dbsql

def dict_factory(cursor, row):
    d = {}
    for idx, col in enumerate(cursor.description):
        d[col[0]] = row[idx]
    return d

if use_dbsql:
    def dict_factory(cursor, row):
        d = {}
        for idx, col in enumerate(cursor.description):
            d[col[0]] = row[idx]
        return d

    class DictCursor(dbsql.Cursor):
        def __init__(self, *args, **kwargs):
            dbsql.Cursor.__init__(self, *args, **kwargs)
            self.row_factory = dict_factory

    class RowCursor(dbsql.Cursor):
        def __init__(self, *args, **kwargs):
            dbsql.Cursor.__init__(self, *args, **kwargs)
            self.row_factory = dbsql.Row

def create_db():
    if dbsql.version_info > (2, 0):
        if use_custom_types:
            con = dbsql.connect(":memory:", detect_types=dbsql.PARSE_DECLTYPES|dbsql.PARSE_COLNAMES)
            dbsql.register_converter("text", lambda x: "<%s>" % x)
        else:
            con = dbsql.connect(":memory:")
        if use_dictcursor:
            cur = con.cursor(factory=DictCursor)
        elif use_rowcursor:
            cur = con.cursor(factory=RowCursor)
        else:
            cur = con.cursor()
    else:
        if use_tuple:
            con = dbsql.connect(":memory:")
            con.rowclass = tuple
            cur = con.cursor()
        else:
            con = dbsql.connect(":memory:")
            cur = con.cursor()
    cur.execute("""
        create table test(v text, f float, i integer)
        """)
    return (con, cur)

def test():
    row = ("sdfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffasfd", 3.14, 42)
    l = []
    for i in range(1000):
        l.append(row)

    con, cur = create_db()

    if dbsql.version_info > (0, 1):
        sql = "insert into test(v, f, i) values (?, ?, ?)"
    else:
        sql = "insert into test(v, f, i) values (%s, %s, %s)"

    for i in range(50):
        cur.executemany(sql, l)

    cur.execute("select count(*) as cnt from test")

    starttime = time.time()
    for i in range(50):
        cur.execute("select v, f, i from test")
        l = cur.fetchall()
    endtime = time.time()

    print "elapsed:", endtime - starttime

if __name__ == "__main__":
    test()

