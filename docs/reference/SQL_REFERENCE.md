# DBSQL SQL Reference

DBSQL implements a SQL dialect based on SQLite 3.52.0 with SQL-92 compatibility. This reference documents the SQL syntax supported by DBSQL.

## Important: Storage Backend

**DBSQL uses BerkeleyDB for storage**, not SQLite's btree. While the SQL syntax is compatible with SQLite, the underlying storage is provided by BerkeleyDB 5.3.21, which affects:
- File organization (multiple files vs single file)
- Concurrency model (BDB's MVCC)
- Transaction semantics (BDB transactions)
- Replication capabilities (BDB replication available)

## Data Types

DBSQL uses SQLite's manifest typing system with column affinity.

### Type Affinity

Column types are hints, not strict constraints:

| Affinity | Storage Classes | Example Types |
|----------|----------------|---------------|
| INTEGER | INTEGER | INT, INTEGER, TINYINT, BIGINT |
| REAL | REAL | REAL, DOUBLE, FLOAT |
| TEXT | TEXT | VARCHAR, CHARACTER, TEXT, CLOB |
| BLOB | BLOB | BLOB |
| NUMERIC | INTEGER or REAL | NUMERIC, DECIMAL, BOOLEAN, DATE |

### Storage Classes

Every value stored in DBSQL has one of these storage classes:

- **NULL**: The null value
- **INTEGER**: Signed 64-bit integer (-2^63 to 2^63-1)
- **REAL**: IEEE 754 floating point (64-bit)
- **TEXT**: UTF-8 encoded string (up to 1 GB)
- **BLOB**: Binary data (up to 1 GB)

### Type Conversions

Automatic conversions occur based on context:

```sql
-- Integer to TEXT
SELECT 42 || ' items';  -- '42 items'

-- TEXT to INTEGER
SELECT '42' + 8;  -- 50

-- TEXT to REAL
SELECT '3.14' * 2;  -- 6.28
```

## SQL Statements

### SELECT

Retrieve rows from one or more tables.

**Basic Syntax**:
```sql
SELECT [ALL | DISTINCT] column_list
FROM table_references
[WHERE where_condition]
[GROUP BY column_list [HAVING having_condition]]
[ORDER BY column_list [ASC | DESC]]
[LIMIT row_count [OFFSET offset]];
```

**Examples**:
```sql
-- Simple SELECT
SELECT name, email FROM users;

-- With WHERE clause
SELECT name, age FROM users WHERE age > 25;

-- With ORDER BY
SELECT name, age FROM users ORDER BY age DESC;

-- With LIMIT
SELECT name FROM users LIMIT 10;

-- With aggregate
SELECT department, AVG(salary) FROM employees GROUP BY department;

-- With HAVING
SELECT department, AVG(salary) AS avg_sal
FROM employees
GROUP BY department
HAVING avg_sal > 50000;
```

**Joins**:
```sql
-- INNER JOIN
SELECT users.name, orders.total
FROM users
INNER JOIN orders ON users.id = orders.user_id;

-- LEFT OUTER JOIN
SELECT users.name, orders.total
FROM users
LEFT JOIN orders ON users.id = orders.user_id;

-- CROSS JOIN
SELECT * FROM colors CROSS JOIN sizes;
```

**Subqueries**:
```sql
-- Scalar subquery
SELECT name, (SELECT COUNT(*) FROM orders WHERE orders.user_id = users.id) AS order_count
FROM users;

-- IN subquery
SELECT name FROM users WHERE id IN (SELECT user_id FROM orders WHERE total > 100);

-- EXISTS subquery
SELECT name FROM users WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id);
```

### INSERT

Add new rows to a table.

**Syntax**:
```sql
INSERT INTO table_name [(column_list)]
VALUES (value_list) [, (value_list) ...];

INSERT INTO table_name [(column_list)]
SELECT ...;
```

**Examples**:
```sql
-- Single row
INSERT INTO users (name, email, age) VALUES ('Alice', 'alice@example.com', 30);

-- Multiple rows
INSERT INTO users (name, email, age) VALUES
  ('Bob', 'bob@example.com', 25),
  ('Carol', 'carol@example.com', 28);

-- From SELECT
INSERT INTO archived_orders SELECT * FROM orders WHERE created_at < '2020-01-01';

-- DEFAULT values
INSERT INTO users (name) VALUES ('Dave');  -- email and age get DEFAULT or NULL
```

**With RETURNING** ✨ (PostgreSQL-compatible):
```sql
-- Return inserted row
INSERT INTO users (name, email, age)
VALUES ('Alice', 'alice@example.com', 30)
RETURNING id, name;

-- Return multiple columns
INSERT INTO products (name, price)
VALUES ('Widget', 19.99)
RETURNING id, name, price, created_at;
```

### UPDATE

Modify existing rows.

**Syntax**:
```sql
UPDATE table_name
SET column1 = value1 [, column2 = value2 ...]
[WHERE where_condition];
```

**Examples**:
```sql
-- Update single row
UPDATE users SET age = 31 WHERE id = 1;

-- Update multiple columns
UPDATE users SET email = 'newemail@example.com', age = 32 WHERE id = 1;

-- Update with expression
UPDATE products SET price = price * 1.1 WHERE category = 'electronics';

-- Update all rows
UPDATE users SET active = 1;
```

**With RETURNING** ✨:
```sql
-- Return updated values
UPDATE users SET age = age + 1 WHERE id = 1 RETURNING id, name, age;
```

### DELETE

Remove rows from a table.

**Syntax**:
```sql
DELETE FROM table_name [WHERE where_condition];
```

**Examples**:
```sql
-- Delete specific rows
DELETE FROM users WHERE id = 1;

-- Delete with condition
DELETE FROM orders WHERE created_at < '2020-01-01';

-- Delete all rows (use with caution!)
DELETE FROM temp_data;
```

**With RETURNING** ✨:
```sql
-- Return deleted rows
DELETE FROM users WHERE age < 18 RETURNING id, name, email;
```

## Data Definition (DDL)

### CREATE TABLE

Define a new table.

**Syntax**:
```sql
CREATE [TEMP] TABLE [IF NOT EXISTS] table_name (
  column_name data_type [column_constraint] [, ...]
  [, table_constraint [, ...]]
);
```

**Column Constraints**:
- `PRIMARY KEY` - Unique identifier for row
- `NOT NULL` - Column cannot contain NULL
- `UNIQUE` - All values must be unique
- `DEFAULT value` - Default value if not specified
- `CHECK (expression)` - Value must satisfy expression
- `REFERENCES table(column)` - Foreign key constraint

**Examples**:
```sql
-- Simple table
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  email TEXT UNIQUE,
  age INTEGER CHECK (age >= 0)
);

-- With foreign key
CREATE TABLE orders (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL,
  total REAL,
  FOREIGN KEY (user_id) REFERENCES users(id)
);

-- With composite primary key
CREATE TABLE user_roles (
  user_id INTEGER,
  role_id INTEGER,
  PRIMARY KEY (user_id, role_id)
);

-- AUTOINCREMENT
CREATE TABLE products (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  price REAL
);
```

**WITHOUT ROWID**:
```sql
-- Use specified primary key instead of implicit rowid
CREATE TABLE settings (
  key TEXT PRIMARY KEY,
  value TEXT
) WITHOUT ROWID;
```

### CREATE INDEX

Create an index for faster queries.

**Syntax**:
```sql
CREATE [UNIQUE] INDEX [IF NOT EXISTS] index_name
ON table_name (column_list);
```

**Examples**:
```sql
-- Simple index
CREATE INDEX idx_users_email ON users(email);

-- Composite index
CREATE INDEX idx_users_name_age ON users(name, age);

-- Unique index
CREATE UNIQUE INDEX idx_users_email_unique ON users(email);

-- Partial index
CREATE INDEX idx_active_users ON users(name) WHERE active = 1;
```

**Storage Note**: Each index is stored in a separate BerkeleyDB database file.

### DROP TABLE

Remove a table and all its data.

**Syntax**:
```sql
DROP TABLE [IF EXISTS] table_name;
```

**Example**:
```sql
DROP TABLE temp_data;
DROP TABLE IF EXISTS old_archive;
```

**Warning**: This deletes the BerkeleyDB database files. Data cannot be recovered.

### DROP INDEX

Remove an index.

**Syntax**:
```sql
DROP INDEX [IF EXISTS] index_name;
```

**Example**:
```sql
DROP INDEX idx_users_email;
```

## Expressions

### Operators

**Arithmetic**:
- `+` Addition
- `-` Subtraction
- `*` Multiplication
- `/` Division
- `%` Modulo

**Comparison**:
- `=` Equal
- `!=` or `<>` Not equal
- `<` Less than
- `<=` Less than or equal
- `>` Greater than
- `>=` Greater than or equal
- `IS NULL` NULL test
- `IS NOT NULL` Not NULL test

**Logical**:
- `AND` Logical AND
- `OR` Logical OR
- `NOT` Logical NOT

**String**:
- `||` Concatenation
- `LIKE` Pattern matching (% = any chars, _ = one char)
- `GLOB` Pattern matching (Unix-style)

**Examples**:
```sql
-- Arithmetic
SELECT price * 1.1 AS price_with_tax FROM products;

-- Comparison
SELECT name FROM users WHERE age >= 18 AND age < 65;

-- String concatenation
SELECT first_name || ' ' || last_name AS full_name FROM users;

-- Pattern matching
SELECT name FROM users WHERE email LIKE '%@example.com';
```

### Functions

#### Aggregate Functions

| Function | Description |
|----------|-------------|
| `COUNT(*)` | Count rows |
| `COUNT(column)` | Count non-NULL values |
| `SUM(column)` | Sum of values |
| `AVG(column)` | Average of values |
| `MIN(column)` | Minimum value |
| `MAX(column)` | Maximum value |
| `GROUP_CONCAT(column)` | Concatenate values |

**Examples**:
```sql
SELECT COUNT(*) FROM users;
SELECT AVG(age) FROM users;
SELECT department, SUM(salary) FROM employees GROUP BY department;
```

#### Scalar Functions

**String Functions**:
- `LENGTH(string)` - Length in characters
- `LOWER(string)` - Convert to lowercase
- `UPPER(string)` - Convert to uppercase
- `SUBSTR(string, start, length)` - Extract substring
- `TRIM(string)` - Remove whitespace
- `REPLACE(string, old, new)` - Replace substring

**Numeric Functions**:
- `ABS(number)` - Absolute value
- `ROUND(number [, decimals])` - Round number
- `RANDOM()` - Random integer
- `MIN(a, b, ...)` - Minimum of values
- `MAX(a, b, ...)` - Maximum of values

**Type Conversion**:
- `CAST(value AS type)` - Convert type
- `TYPEOF(value)` - Return type name

**Examples**:
```sql
SELECT UPPER(name) FROM users;
SELECT ROUND(price, 2) FROM products;
SELECT CAST(age AS TEXT) FROM users;
```

## Transactions

DBSQL transactions use BerkeleyDB's transaction system.

**Syntax**:
```sql
BEGIN [TRANSACTION];
COMMIT [TRANSACTION];
ROLLBACK [TRANSACTION];
```

**Examples**:
```sql
-- Transfer money between accounts
BEGIN;
  UPDATE accounts SET balance = balance - 100 WHERE id = 1;
  UPDATE accounts SET balance = balance + 100 WHERE id = 2;
COMMIT;

-- Rollback on error
BEGIN;
  INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com');
  -- Oops, duplicate email
  ROLLBACK;
```

**Savepoints**:
```sql
BEGIN;
  INSERT INTO orders (user_id, total) VALUES (1, 100.00);

  SAVEPOINT sp1;
    INSERT INTO order_items (order_id, product_id) VALUES (1, 10);
    -- Decide to undo
    ROLLBACK TO SAVEPOINT sp1;

  INSERT INTO order_items (order_id, product_id) VALUES (1, 11);
COMMIT;
```

**Isolation**: BerkeleyDB provides snapshot isolation. Transactions see a consistent view of data at BEGIN time.

## Views

Create named queries for reuse.

**Syntax**:
```sql
CREATE [TEMP] VIEW [IF NOT EXISTS] view_name AS
SELECT ...;
```

**Examples**:
```sql
-- Simple view
CREATE VIEW active_users AS
SELECT * FROM users WHERE active = 1;

-- Use view
SELECT name FROM active_users WHERE age > 25;

-- Complex view
CREATE VIEW user_order_summary AS
SELECT
  users.name,
  COUNT(orders.id) AS order_count,
  SUM(orders.total) AS total_spent
FROM users
LEFT JOIN orders ON users.id = orders.user_id
GROUP BY users.id;
```

**Drop View**:
```sql
DROP VIEW active_users;
```

## Triggers

Execute SQL automatically in response to events.

**Syntax**:
```sql
CREATE [TEMP] TRIGGER [IF NOT EXISTS] trigger_name
[BEFORE | AFTER | INSTEAD OF] [INSERT | UPDATE | DELETE]
ON table_name
[FOR EACH ROW]
[WHEN condition]
BEGIN
  sql_statements;
END;
```

**Examples**:
```sql
-- Update timestamp
CREATE TRIGGER update_timestamp
AFTER UPDATE ON users
FOR EACH ROW
BEGIN
  UPDATE users SET updated_at = CURRENT_TIMESTAMP WHERE id = NEW.id;
END;

-- Audit log
CREATE TRIGGER audit_delete
AFTER DELETE ON users
FOR EACH ROW
BEGIN
  INSERT INTO audit_log (table_name, row_id, action, timestamp)
  VALUES ('users', OLD.id, 'DELETE', CURRENT_TIMESTAMP);
END;

-- Validate data
CREATE TRIGGER check_price
BEFORE INSERT ON products
FOR EACH ROW
WHEN NEW.price < 0
BEGIN
  SELECT RAISE(ABORT, 'Price cannot be negative');
END;
```

**Drop Trigger**:
```sql
DROP TRIGGER update_timestamp;
```

## ATTACH/DETACH

Work with multiple databases in one connection.

**Syntax**:
```sql
ATTACH [DATABASE] filename AS schema_name;
DETACH [DATABASE] schema_name;
```

**Examples**:
```sql
-- Attach second database
ATTACH DATABASE 'archive.db' AS archive;

-- Query across databases
SELECT users.name, archive.old_orders.total
FROM users
JOIN archive.old_orders ON users.id = archive.old_orders.user_id;

-- Detach
DETACH DATABASE archive;
```

**Storage Note**: Each attached database is a separate BerkeleyDB environment with its own files.

## PRAGMA

Configure database settings.

**Common PRAGMAs**:
```sql
-- Show schema
PRAGMA table_info(users);

-- List tables
PRAGMA table_list;

-- Show indexes
PRAGMA index_list(users);

-- Foreign key enforcement (note: BDB storage layer doesn't enforce)
PRAGMA foreign_keys = ON;

-- Database integrity check
PRAGMA integrity_check;
```

**Storage Configuration** (BDB-specific, via environment):
- Cache size: Configured in BDB environment
- Page size: Configured when creating BDB databases
- Transaction logs: BDB manages automatically

## Differences from SQLite

DBSQL is compatible with SQLite SQL syntax, but has these differences due to BerkeleyDB storage:

### 1. File Organization

**SQLite**: Single file database
**DBSQL**: Multiple BDB database files (one per table/index)

### 2. Concurrency

**SQLite**: Multi-reader, single-writer (WAL mode similar)
**DBSQL**: Multi-reader, single-writer (BDB MVCC)

Both: Readers never block writers or other readers

### 3. Transaction Semantics

**SQLite**: Deferred transactions (lock on first write)
**DBSQL**: Immediate transactions (BDB transaction starts at BEGIN)

### 4. File Format

**SQLite**: Proprietary btree format
**DBSQL**: BerkeleyDB B-tree format

**Implication**: Cannot open SQLite databases with DBSQL or vice versa. Migration requires `dump/load`.

### 5. Replication

**SQLite**: No built-in replication
**DBSQL**: BerkeleyDB replication available (master-replica)

### 6. Type System

**Both**: Manifest typing with column affinity (identical)

### 7. SQL Syntax

**Mostly identical**, with these DBSQL additions:
- `RETURNING` clause (PostgreSQL-compatible)

Planned additions:
- `UPSERT` (INSERT ON CONFLICT)
- Window functions (ROW_NUMBER, RANK, etc.)
- Recursive CTEs (WITH RECURSIVE)

## Performance Considerations

### Use Indexes

```sql
-- Slow: Full table scan
SELECT name FROM users WHERE email = 'alice@example.com';

-- Fast: Index lookup
CREATE INDEX idx_users_email ON users(email);
SELECT name FROM users WHERE email = 'alice@example.com';
```

### Batch Operations

```sql
-- Slow: Many small transactions
BEGIN;
  INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');
COMMIT;
BEGIN;
  INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');
COMMIT;

-- Fast: One transaction
BEGIN;
  INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');
  INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');
COMMIT;
```

### Use RETURNING Instead of SELECT

```sql
-- Slow: Separate queries
INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com');
SELECT id FROM users WHERE name = 'Alice' AND email = 'alice@example.com';

-- Fast: Single query
INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com') RETURNING id;
```

## Migration from SQLite

### Export from SQLite

```bash
sqlite3 mydb.db .dump > dump.sql
```

### Import to DBSQL

```bash
dbsql newdb.db < dump.sql
```

**Notes**:
- Schema is preserved (tables, indexes, triggers, views)
- Data is migrated
- Adjust any SQLite-specific features (e.g., virtual tables not supported)

## Related Documentation

- [C API Reference](../api/C_API.md) - Using DBSQL from C
- [Storage Guide](../guides/STORAGE.md) - BerkeleyDB storage details
- [Architecture](../architecture/ARCHITECTURE.md) - How DBSQL works internally
