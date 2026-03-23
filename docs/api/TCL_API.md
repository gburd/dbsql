# DBSQL TCL Extension API

DBSQL provides a TCL extension that allows scripts to interact with databases using native TCL syntax.

## Overview

The TCL extension wraps DBSQL's C API, providing:
- Database connection management
- SQL statement execution
- Result set processing
- Transaction control
- Prepared statements

**Installation**: Build DBSQL with `--enable-tcl` flag.

## Loading the Extension

```tcl
# Load DBSQL extension
load /usr/local/lib/libdbsql-0.4.so dbsql

# Or if installed in standard location
package require dbsql
```

## Basic Usage

### Opening a Database

```tcl
# Create/open database
set db [dbsql_open "myapp.db"]

# In-memory database
set db [dbsql_open ":memory:"]
```

**Returns**: Database handle (used in subsequent commands)

### Executing SQL

```tcl
# Execute SQL without result set
$db eval "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)"

# Execute with error handling
if {[catch {$db eval "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com')"} err]} {
    puts "Error: $err"
}
```

### Querying Data

```tcl
# Query with callback
$db eval "SELECT name, email FROM users" values {
    puts "Name: $values(name), Email: $values(email)"
}

# Query into array
set results [$db eval "SELECT * FROM users"]
# Returns: {1 Alice alice@example.com 2 Bob bob@example.com}

# Query with column names
set results [$db eval "SELECT name, email FROM users"]
foreach {name email} $results {
    puts "$name <$email>"
}
```

### Closing Database

```tcl
$db close
```

## Commands

### dbsql_open

Open or create a database.

```tcl
set db [dbsql_open filename]
```

**Arguments**:
- `filename`: Path to database file, or `:memory:` for in-memory database

**Returns**: Database handle

**Example**:
```tcl
set db [dbsql_open "test.db"]
if {$db == ""} {
    error "Failed to open database"
}
```

### $db eval

Execute SQL statement.

```tcl
$db eval sql [array_name] [script]
```

**Arguments**:
- `sql`: SQL statement to execute
- `array_name`: (Optional) Name of array to receive column values
- `script`: (Optional) TCL script executed for each result row

**Returns**:
- Without script: List of column values
- With script: Result of last script execution

**Examples**:

**Simple execution**:
```tcl
$db eval "CREATE TABLE t1 (a INTEGER, b TEXT)"
$db eval "INSERT INTO t1 VALUES (1, 'hello')"
```

**Query with array**:
```tcl
$db eval "SELECT a, b FROM t1" row {
    puts "a=$row(a), b=$row(b)"
}
```

**Query returning list**:
```tcl
set values [$db eval "SELECT a, b FROM t1"]
# Returns: {1 hello}
```

**Multiple rows**:
```tcl
$db eval "SELECT name, age FROM users ORDER BY name" user {
    puts "$user(name) is $user(age) years old"
}
```

### $db close

Close database connection.

```tcl
$db close
```

**Example**:
```tcl
$db close
unset db
```

### $db changes

Get number of rows modified by last statement.

```tcl
set count [$db changes]
```

**Returns**: Number of rows inserted, updated, or deleted

**Example**:
```tcl
$db eval "UPDATE users SET active = 1 WHERE age > 18"
puts "Updated [$db changes] rows"
```

### $db last_insert_rowid

Get rowid of last inserted row.

```tcl
set rowid [$db last_insert_rowid]
```

**Returns**: 64-bit integer rowid

**Example**:
```tcl
$db eval "INSERT INTO users (name) VALUES ('Alice')"
set id [$db last_insert_rowid]
puts "Inserted row with id: $id"
```

### $db errorcode

Get error code from last operation.

```tcl
set code [$db errorcode]
```

**Returns**: Integer error code (see DBSQL_* constants)

### $db errmsg

Get error message from last operation.

```tcl
set msg [$db errmsg]
```

**Returns**: UTF-8 error message string

**Example**:
```tcl
if {[catch {$db eval "INVALID SQL"} err]} {
    puts "Error [$db errorcode]: [$db errmsg]"
}
```

## Prepared Statements

For repeated execution with different parameters.

### $db prepare

Create prepared statement.

```tcl
set stmt [$db prepare sql]
```

**Arguments**:
- `sql`: SQL statement with `?` placeholders

**Returns**: Statement handle

**Example**:
```tcl
set stmt [$db prepare "INSERT INTO users (name, email) VALUES (?, ?)"]
```

### $stmt bind

Bind parameters to statement.

```tcl
$stmt bind index value [index value ...]
```

**Arguments**:
- `index`: Parameter index (1-based)
- `value`: Value to bind

**Example**:
```tcl
$stmt bind 1 "Alice" 2 "alice@example.com"
```

### $stmt step

Execute one step of statement.

```tcl
set result [$stmt step]
```

**Returns**:
- `DBSQL_ROW`: Row available (call again for next row)
- `DBSQL_DONE`: Execution complete
- Error code: Execution failed

**Example**:
```tcl
while {[$stmt step] == $DBSQL_ROW} {
    # Process row
    set name [$stmt column 0]
    puts $name
}
```

### $stmt column

Get column value from current row.

```tcl
set value [$stmt column index]
```

**Arguments**:
- `index`: Column index (0-based)

**Returns**: Column value

**Example**:
```tcl
set name [$stmt column 0]
set email [$stmt column 1]
set age [$stmt column 2]
```

### $stmt reset

Reset statement to beginning.

```tcl
$stmt reset
```

Use to re-execute statement with different parameters.

**Example**:
```tcl
$stmt bind 1 "Alice" 2 "alice@example.com"
$stmt step

$stmt reset
$stmt bind 1 "Bob" 2 "bob@example.com"
$stmt step
```

### $stmt finalize

Destroy prepared statement.

```tcl
$stmt finalize
```

**Example**:
```tcl
$stmt finalize
unset stmt
```

## Transaction Management

```tcl
# Begin transaction
$db eval "BEGIN"

# Execute statements
$db eval "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com')"
$db eval "UPDATE accounts SET balance = balance - 100 WHERE user_id = 1"

# Commit
$db eval "COMMIT"

# OR rollback on error
if {[catch {
    $db eval "BEGIN"
    # ... statements ...
    $db eval "COMMIT"
} err]} {
    $db eval "ROLLBACK"
    puts "Transaction failed: $err"
}
```

## Complete Example

```tcl
#!/usr/bin/env tclsh

# Load DBSQL extension
package require dbsql

# Open database
set db [dbsql_open "example.db"]

# Create table
$db eval {
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        email TEXT UNIQUE,
        age INTEGER
    )
}

# Insert data
$db eval "BEGIN"
$db eval "INSERT INTO users (name, email, age) VALUES ('Alice', 'alice@example.com', 30)"
$db eval "INSERT INTO users (name, email, age) VALUES ('Bob', 'bob@example.com', 25)"
$db eval "INSERT INTO users (name, email, age) VALUES ('Carol', 'carol@example.com', 28)"
$db eval "COMMIT"

puts "Inserted [$db changes] rows"

# Query data
puts "\nUsers over 25:"
$db eval "SELECT name, email, age FROM users WHERE age > 25 ORDER BY name" user {
    puts "  $user(name) <$user(email)> (age $user(age))"
}

# Update with RETURNING
puts "\nUpdating ages:"
$db eval "UPDATE users SET age = age + 1 WHERE age < 30 RETURNING name, age" user {
    puts "  $user(name) is now $user(age)"
}

# Prepared statement
set stmt [$db prepare "SELECT name, age FROM users WHERE age >= ?"]
$stmt bind 1 28

puts "\nUsers age >= 28:"
while {[$stmt step] == $::DBSQL_ROW} {
    set name [$stmt column 0]
    set age [$stmt column 1]
    puts "  $name: $age"
}

$stmt finalize

# Cleanup
$db close
```

**Run**:
```bash
tclsh example.tcl
```

## Using RETURNING Clause

```tcl
# INSERT RETURNING
$db eval "INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com') RETURNING id" row {
    puts "Inserted with id: $row(id)"
}

# UPDATE RETURNING
$db eval "UPDATE users SET age = age + 1 WHERE name = 'Alice' RETURNING age" row {
    puts "New age: $row(age)"
}

# DELETE RETURNING
$db eval "DELETE FROM users WHERE age < 18 RETURNING id, name" row {
    puts "Deleted user $row(id): $row(name)"
}
```

## Error Handling

```tcl
# Try-catch
if {[catch {$db eval "INVALID SQL"} err]} {
    puts "SQL Error: $err"
    puts "Code: [$db errorcode]"
    puts "Message: [$db errmsg]"
}

# Check return codes
set stmt [$db prepare "SELECT * FROM users WHERE age > ?"]
$stmt bind 1 25

set rc [$stmt step]
if {$rc == $::DBSQL_ROW} {
    # Process row
    puts [$stmt column 0]
} elseif {$rc == $::DBSQL_DONE} {
    # No more rows
    puts "No results"
} else {
    # Error
    puts "Error: [$db errmsg]"
}

$stmt finalize
```

## Constants

The extension defines these constants:

```tcl
$::DBSQL_OK          ;# Success
$::DBSQL_ERROR       ;# SQL error
$::DBSQL_BUSY        ;# Database locked
$::DBSQL_NOMEM       ;# Out of memory
$::DBSQL_CONSTRAINT  ;# Constraint violation
$::DBSQL_ROW         ;# Row available
$::DBSQL_DONE        ;# Execution complete
```

## Utility Procedures

### Checking Existence

```tcl
# Check if table exists
proc table_exists {db table_name} {
    set count [$db eval "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='$table_name'"]
    return [expr {$count > 0}]
}

if {[table_exists $db "users"]} {
    puts "Table exists"
}
```

### Safe Query

```tcl
# Execute query with error handling
proc safe_eval {db sql} {
    if {[catch {$db eval $sql} result]} {
        error "SQL error: $result"
    }
    return $result
}

safe_eval $db "CREATE TABLE t1 (id INTEGER PRIMARY KEY)"
```

### Batch Insert

```tcl
# Insert multiple rows efficiently
proc batch_insert {db table data} {
    $db eval "BEGIN"
    foreach row $data {
        set values [join $row "','"]
        $db eval "INSERT INTO $table VALUES ('$values')"
    }
    $db eval "COMMIT"
    return [$db changes]
}

set data {
    {1 Alice alice@example.com}
    {2 Bob bob@example.com}
    {3 Carol carol@example.com}
}

set count [batch_insert $db "users" $data]
puts "Inserted $count rows"
```

## Differences from SQLite TCL API

DBSQL's TCL extension is compatible with SQLite's TCL API, with these notes:

1. **Storage**: Uses BerkeleyDB (multiple files vs single file)
2. **Functions**: Core functions identical
3. **Extensions**: DBSQL-specific features (RETURNING clause)
4. **Performance**: BDB concurrency model (single writer, multi-reader)

**Migration**: Most SQLite TCL code works unchanged with DBSQL.

## Test Suite Usage

The test suite uses the TCL extension extensively:

```tcl
# test/testutils.tcl
proc db_open {filename} {
    global db
    set db [dbsql_open $filename]
    return $db
}

proc db_exec {sql} {
    global db
    $db eval $sql
}

proc db_query {sql} {
    global db
    return [$db eval $sql]
}

proc error_check_good {name actual expected} {
    if {$actual != $expected} {
        error "FAIL: $name - expected '$expected', got '$actual'"
    }
}
```

## Related Documentation

- [C API Reference](C_API.md) - C API (TCL extension wraps this)
- [SQL Reference](../reference/SQL_REFERENCE.md) - SQL syntax
- [Testing Guide](../TESTING.md) - Test suite using TCL
- [TCL Documentation](https://www.tcl.tk/man/) - TCL language reference
