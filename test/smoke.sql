CREATE TABLE Person (
       id    NUMBER(3),
       fname TEXT(20),
       lname TEXT(40),
       ssn   TEXT(11));
CREATE TABLE Address (
       person_fk   NUMBER(3),
       first_line  TEXT(30),
       second_line TEXT(30),
       city        TEXT(30),
       state       TEXT(2),
       zip         TEXT(5));
INSERT INTO Person VALUES (1,"John", "Doe", "111-22-3333");
INSERT INTO Person VALUES (2, "Jane", "Doe", "222-33-4444");
INSERT INTO Address VALUES (1, "20 First St.", "# 2", "Boston", "MA", "02110");

select city from Person, Address where person_fk = id and id = 1;

