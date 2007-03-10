import time

def yesno(question):
    val = raw_input(question + " ")
    return val.startswith("y") or val.startswith("Y")

use_pydbsql = yesno("Use pydbsql?")
use_autocommit = yesno("Use autocommit?")
use_executemany= yesno("Use executemany?")

if use_pydbsql:
    from pydbsql import dbapi2 as dbsql
else:
    import dbsql


def create_db():
    con = dbsql.connect(":memory:")
    if use_autocommit:
        if use_pydbsql:
            con.isolation_level = None
        else:
            con.autocommit = True
    cur = con.cursor()
    cur.execute("""
        create table test(v text, f float, i integer)
        """)
    cur.close()
    return con

def test():
    row = ("sdfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffasfd", 3.14, 42)
    l = []
    for i in range(1000):
        l.append(row)

    con = create_db()
    cur = con.cursor()

    if dbsql.version_info > (2, 0):
        sql = "insert into test(v, f, i) values (?, ?, ?)"
    else:
        sql = "insert into test(v, f, i) values (%s, %s, %s)"

    starttime = time.time()
    for i in range(50):
        if use_executemany:
            cur.executemany(sql, l)
        else:
            for r in l:
                cur.execute(sql, r)
    endtime = time.time()

    print "elapsed", endtime - starttime

    cur.execute("select count(*) from test")
    print "rows:", cur.fetchone()[0]

if __name__ == "__main__":
    test()

