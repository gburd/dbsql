.headers on
.mode box
create table t(a integer primary key, b text);
insert into t(b) values ('x'),('y');
select * from t;
.import --csv t.csv people
.mode json
select * from people order by id;
.mode markdown
select name, row_number() over (order by name desc) rn from people;
create virtual table ft using fts5(body);
insert into ft values ('the quick brown fox'),('lazy dog');
select rowid, body from ft where ft match 'quick';
.mode list
select dbsql_version() is not null, json_object('k', 1), '{"a":5}' ->> '$.a';
select upper(sqlite_source_id()) is null;
.timer on
select count(*) from people;
.timer off
.print done
