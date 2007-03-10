
CREATE TABLE COMPANIES (
       companyname    TEXT(20),
       address        TEXT(40),
       city           TEXT(20));
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 First St",  "Boston");
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 First St",  "Baltimore");
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 Second St", "Bangor");
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 Third St",  "Beantown");
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 Fourth St", "BatCave");
INSERT INTO COMPANIES VALUES ("Sleepycat", "123 Fifth St",  "Boolcarp");

INSERT INTO COMPANIES VALUES ("Slentek", "123 Fifth St",  "Boolcarp");
INSERT INTO COMPANIES VALUES ("Slentek", "123 Fifth St",  "Boolcarp");
INSERT INTO COMPANIES VALUES ("Slentek", "123 Fifth St",  "Boolcarp");
INSERT INTO COMPANIES VALUES ("Slentek", "123 Fifth St",  "Boolcarp");

INSERT INTO COMPANIES VALUES ("Sleze", "123 Jae Ave",  "Bangor");

INSERT INTO COMPANIES VALUES ("Kaker", "123 Jae Ave",  "Bangor");

SELECT companyname, COUNT(*) FROM COMPANIES
WHERE companyname LIKE 'Sle%'
  AND address LIKE '123%'
  AND city LIKE 'B%'
GROUP BY companyname
HAVING COUNT(*) >= 1;

