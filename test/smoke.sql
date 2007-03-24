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

CREATE TABLE Phone (
	person_fk  NUMBER(3),
	type       TEXT(20),
	number     TEXT(15));

INSERT INTO Person VALUES (1,"John", "Doe", "111-22-3333");
INSERT INTO Person VALUES (2, "Jane", "Doe", "222-33-4444");
INSERT INTO Person VALUES (3, "Judy", "Schaab", "888-22-0001");

INSERT INTO Address VALUES (1, "20 First St.", "# 2", "Boston", "MA", "02110");
INSERT INTO Address VALUES (2, "20 First St.", "# 2", "Boston", "MA", "02110");
INSERT INTO Address VALUES (3, "11600 Academy Rd NE", "Apt 4511", "Albuquerque", "NM", "87111");

INSERT INTO Phone VALUES (1, "home", "617-123-4567");
INSERT INTO Phone VALUES (2, "cell", "617-222-8777");
INSERT INTO Phone VALUES (3, "home", "505-243-4670");
INSERT INTO Phone VALUES (3, "cell", "505-259-7789");

select city from Person, Address where person_fk = id and id = 1;
select number from Person, Phone where person_fk = id and fname = "Judy" and lname = "Schaab";
select * from Phone where type = "cell";
select number from Person, Phone where person_fk = id and fname = "Judy" and lname = "Schaab" and type = "cell";
select number from Person, Phone where person_fk = id and ssn = "888-22-0001" and type = "cell";