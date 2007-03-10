create table t1 (b, c);
  insert into t1 values ('dog',3);
  insert into t1 values ('cat',1);
  insert into t1 values ('dog',4);

create table t2 (c, e);
  insert into t2 values (1,'one');
  insert into t2 values (2,'two');
  insert into t2 values (3,'three');
  insert into t2 values (4,'four');

select * from ( select  t2.c as c, e, b from t2 left join (select  b, max(c) as c from t1 group by b) using (c) );
