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
.headers off
select dbsql_version() is not null, json_object('k', 1), '{"a":5}' ->> '$.a';
select count(*) from dbsql_schema;
select icu_load_collation('en_US', 'english') is null;
-- ICU: under the English collation 'a' < 'B' (binary: 'B' < 'a').
select 'a' < 'B' collate english, 'a' < 'B';
.print done
