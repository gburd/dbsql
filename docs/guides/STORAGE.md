# BerkeleyDB Storage Guide

DBSQL uses BerkeleyDB 5.3.21 as its storage engine, providing ACID transactions, concurrent access, and replication capabilities.

## Key Concept: DBSQL Does NOT Use SQLite's Btree

**Important**: DBSQL uses BerkeleyDB for all storage operations. It does not use SQLite's btree implementation. While DBSQL adopts SQLite's SQL parser and register-based VDBE architecture, all physical storage is handled by BerkeleyDB.

## Storage Architecture

### Database Files

When you open a DBSQL database, BerkeleyDB creates multiple files:

```bash
# Example: database "myapp.db"
myapp.db                    # Main database container
├── __DBSQL_meta__          # Schema metadata (table definitions)
├── __DBSQL_users_dbi__     # Sequence for "users" table rowid allocation
├── __DBSQL_products_dbi__  # Sequence for "products" table
├── table_2.db              # Physical storage for "users" table
├── table_3.db              # Physical storage for "products" table
└── index_4.db              # Physical storage for index on users(email)

# Transaction logs
log.0000000001              # Write-ahead log (active)
log.0000000002              # Write-ahead log (archived)

# BerkeleyDB metadata
__db.001                    # Lock tables
__db.002                    # Memory pool metadata
__db.003                    # Transaction region
```

### Storage Mapping

#### SQL Tables → BDB Databases

Each SQL table is stored in a separate BDB database file:

```sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT,
    email TEXT,
    age INTEGER
);
```

**Physical Storage**:
- File: `table_N.db` (where N is allocated from sequence)
- Format: BDB B-tree database
- Keys: 64-bit rowid (big-endian, 8 bytes)
- Values: Row data (all columns packed together)

**Row Format** (internal):
```
[name_len][name_data][email_len][email_data][age_value]
```

#### SQL Indexes → BDB Databases

Each SQL index is a separate BDB database:

```sql
CREATE INDEX idx_users_email ON users(email);
```

**Physical Storage**:
- File: `index_M.db`
- Format: BDB B-tree with duplicates enabled
- Keys: Index key (email value)
- Values: Rowid of matching row (8 bytes)

**For composite indexes**:
```sql
CREATE INDEX idx_users_name_age ON users(name, age);
```
- Keys: Concatenated (name + age), properly formatted
- Allows efficient range scans on prefixes

### Schema Metadata

Schema information is stored in `__DBSQL_meta__`:

```
Key: "table:users"
Value: CREATE TABLE statement SQL text

Key: "index:idx_users_email"
Value: CREATE INDEX statement SQL text

Key: "schema_version"
Value: "1"
```

This allows DBSQL to reconstruct the schema when opening a database.

## BerkeleyDB Data Structures

### B-tree

DBSQL uses B-tree for all tables and indexes:

**Characteristics**:
- Sorted by key (enables ORDER BY optimization)
- Balanced tree (O(log N) lookup, insert, delete)
- Efficient range scans (via cursors)
- Supports duplicate keys (for indexes)

**Page Size**: Default 4096 bytes (tunable)

**Fanout**: Typically 100-200 child pointers per internal node

**Depth**: 3-4 levels for millions of rows

### Cursors

BerkeleyDB cursors provide positioned access:

```c
DBC *cursor;
db->cursor(db, txn, &cursor, 0);

// Iterate through all rows
while (cursor->get(cursor, &key, &data, DB_NEXT) == 0) {
    // Process row
}

cursor->close(cursor);
```

**Cursor Operations**:
- `DB_FIRST` - Move to first key
- `DB_LAST` - Move to last key
- `DB_NEXT` - Advance to next key
- `DB_PREV` - Move to previous key
- `DB_SET` - Seek to specific key
- `DB_SET_RANGE` - Seek to key >= target

DBSQL's VDBE uses these operations for:
- Table scans (OP_Rewind, OP_Next)
- Index seeks (OP_SeekGE, OP_SeekLE)
- Positioned updates/deletes

## Transactions

### ACID Properties

BerkeleyDB provides full ACID guarantees:

**Atomicity**: All operations in a transaction commit together or none at all.

**Consistency**: Constraints (CHECK, FOREIGN KEY) enforced at SQL layer.

**Isolation**: Snapshot isolation via MVCC (Multi-Version Concurrency Control).

**Durability**: Write-ahead logging ensures committed data survives crashes.

### Transaction Lifecycle

```c
// Begin transaction
dbsql_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
// Internally: env->txn_begin(env, NULL, &txn, 0)

// Execute statements
dbsql_exec(db, "INSERT INTO users ...", NULL, NULL, NULL);
dbsql_exec(db, "UPDATE products ...", NULL, NULL, NULL);

// Commit
dbsql_exec(db, "COMMIT", NULL, NULL, NULL);
// Internally: txn->commit(txn, 0)

// OR rollback on error
dbsql_exec(db, "ROLLBACK", NULL, NULL, NULL);
// Internally: txn->abort(txn)
```

### Transaction Isolation

BerkeleyDB provides **Snapshot Isolation**:
- Each transaction sees a consistent snapshot of the database
- Readers never block writers
- Writers never block readers
- Writers block other writers (single-writer model)

**Read Committed** (default):
```sql
BEGIN;
  SELECT * FROM users;  -- Sees committed data at BEGIN time
  -- Other transactions' commits are not visible
COMMIT;
```

**Dirty Reads** (not supported):
DBSQL does not support READ UNCOMMITTED. All reads see only committed data.

### Savepoints

BerkeleyDB supports nested savepoints:

```sql
BEGIN;
  INSERT INTO users VALUES (1, 'Alice', 'alice@example.com', 30);

  SAVEPOINT sp1;
    INSERT INTO users VALUES (2, 'Bob', 'bob@example.com', 25);
    -- Decide to undo Bob
    ROLLBACK TO SAVEPOINT sp1;

  INSERT INTO users VALUES (3, 'Carol', 'carol@example.com', 28);
COMMIT;
-- Result: Alice and Carol inserted, Bob rolled back
```

## Concurrency

### MVCC (Multi-Version Concurrency Control)

BerkeleyDB uses MVCC to allow concurrent readers and writers:

**How it works**:
1. Each transaction gets a unique transaction ID
2. Updates create new versions of rows (old version retained)
3. Readers see versions visible to their snapshot
4. Background thread purges old versions

**Benefits**:
- Unlimited concurrent readers
- Readers never wait for writers
- Writers never wait for readers
- No read locks

**Tradeoff**: Writers still block writers (single-writer model)

### Locking

BerkeleyDB uses page-level and row-level locks:

**Read Locks** (acquired by readers):
- Shared lock on page
- Released when cursor moves off page
- Never conflicts with other readers
- Conflicts with write locks

**Write Locks** (acquired by writers):
- Exclusive lock on page
- Held until transaction commits/aborts
- Conflicts with all other locks
- Deadlock detection enabled

### Deadlock Handling

BerkeleyDB detects and resolves deadlocks:

```c
// Configuration
env->set_lk_detect(env, DB_LOCK_DEFAULT);

// When deadlock occurs:
// - One transaction aborted (victim selection)
// - Application receives DB_LOCK_DEADLOCK error
// - Application should retry transaction
```

DBSQL automatically retries on deadlock (up to 3 attempts).

## Write-Ahead Logging (WAL)

### Log Files

BerkeleyDB writes all changes to transaction logs before modifying data files:

```
log.0000000001   # 10 MB
log.0000000002   # 10 MB (current)
```

**Log Rotation**: Automatic at 10 MB (configurable)

**Log Format**: Binary format with:
- Transaction ID
- Before-image (for undo)
- After-image (for redo)
- LSN (Log Sequence Number)

### Recovery

On database open, BerkeleyDB automatically:
1. Scans transaction logs
2. Redoes committed transactions (roll-forward)
3. Undoes uncommitted transactions (rollback)

**Crash Recovery Example**:
```
1. Transaction A: BEGIN, INSERT row 1, INSERT row 2, COMMIT [logged]
2. Transaction B: BEGIN, INSERT row 3, [no COMMIT logged]
3. CRASH
4. Recovery: Redo A (rows 1, 2), Undo B (rollback row 3)
```

### Checkpoints

Periodic checkpoints reduce recovery time:

```c
// Automatic checkpointing
env->set_flags(env, DB_TXN_WRITE_NOSYNC, 0);
env->txn_checkpoint(env, 1024, 5, 0);  // Every 1 MB or 5 minutes
```

**Checkpoint Process**:
1. Flush all dirty pages to data files
2. Write checkpoint record to log
3. Old logs can be archived/deleted

## Performance Tuning

### Cache Size

BerkeleyDB cache dramatically improves performance:

```c
// Set 128 MB cache
DB_ENV *env;
env_create(&env, 0);
env->set_cachesize(env, 0, 128 * 1024 * 1024, 1);
env->open(env, "/path/to/db", DB_CREATE | DB_INIT_MPOOL | DB_INIT_TXN, 0);
```

**Sizing Guidelines**:
- Minimum: 1 MB per database
- Typical: 10-20% of database size
- Maximum: 50% of available RAM
- Monitor: `db_stat -m` shows hit rate

**Hit Rate Formula**:
```
Hit Rate = (Requests - Misses) / Requests
```
Target: > 95% for good performance

### Page Size

Tune page size based on row size:

```c
DB *db;
db_create(&db, env, 0);
db->set_pagesize(db, 8192);  // 8 KB pages
```

**Guidelines**:
| Average Row Size | Page Size |
|------------------|-----------|
| < 100 bytes | 4 KB |
| 100-500 bytes | 8 KB |
| 500-2000 bytes | 16 KB |
| > 2000 bytes | 32 KB |

### Bulk Operations

Use bulk insert for better performance:

```sql
BEGIN;
  INSERT INTO users VALUES (1, 'Alice', 'alice@example.com', 30);
  INSERT INTO users VALUES (2, 'Bob', 'bob@example.com', 25);
  -- ... thousands of rows ...
COMMIT;  -- One transaction = one fsync
```

**Performance**:
- Per-row commit: 100-500 inserts/sec
- Bulk commit: 10,000-50,000 inserts/sec

### Indexes

Create indexes after bulk loading:

```sql
-- Load data without indexes
CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT);
INSERT INTO users SELECT * FROM source_data;  -- Millions of rows

-- Then create indexes
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_name ON users(name);
```

**Rationale**: Building index on loaded data is faster than maintaining during inserts.

## Backup and Recovery

### Hot Backup

BerkeleyDB supports hot backups (no downtime):

```bash
# Using db_hotbackup utility
db_hotbackup -h /path/to/db -b /path/to/backup
```

**Process**:
1. Copy all database files
2. Copy transaction logs
3. Backup is consistent (can be restored)

### Archive Logs

Archive old transaction logs for point-in-time recovery:

```bash
# Enable log archiving
db_archive -s  # List logs safe to archive
db_archive -d  # Delete archived logs
```

**Retention Policy**:
- Keep logs for recovery window (e.g., 7 days)
- Backup logs offsite
- Rotate regularly

### Restore

```bash
# 1. Restore database files
cp -r /path/to/backup/* /path/to/db/

# 2. Restore archived logs
cp /archive/log.* /path/to/db/

# 3. Open database (automatic recovery)
dbsql myapp.db
```

BerkeleyDB applies logs to bring database to consistent state.

## Replication

BerkeleyDB provides master-replica replication:

**Architecture**:
```
┌────────────┐
│   Master   │  Accepts writes
└─────┬──────┘
      │ Replication Stream (log records)
      ├──────────┬──────────┐
      ↓          ↓          ↓
┌──────────┐ ┌──────────┐ ┌──────────┐
│ Replica1 │ │ Replica2 │ │ Replica3 │  Read-only
└──────────┘ └──────────┘ └──────────┘
```

**Features**:
- Asynchronous replication (low latency)
- Automatic failover (elect new master)
- Load balancing (read from replicas)

**Configuration** (simplified):
```c
// Master
env->repmgr_set_local_site(env, "localhost", 5000, 0);
env->repmgr_start(env, 3, DB_REP_MASTER);

// Replica
env->repmgr_set_local_site(env, "localhost", 5001, 0);
env->repmgr_add_remote_site(env, "localhost", 5000, NULL, 0);
env->repmgr_start(env, 3, DB_REP_CLIENT);
```

See BerkeleyDB documentation for complete replication setup.

## Monitoring

### Statistics

Query BerkeleyDB statistics:

```bash
# Cache statistics
db_stat -m -h /path/to/db

# Lock statistics
db_stat -l -h /path/to/db

# Transaction statistics
db_stat -t -h /path/to/db

# Log statistics
db_stat -L -h /path/to/db
```

### Key Metrics

**Cache Hit Rate**:
```
Requested pages: 1000000
Cache misses: 5000
Hit rate: 99.5%  (excellent)
```

**Lock Contention**:
```
Lock requests: 50000
Lock waits: 100
Contention: 0.2%  (acceptable)
```

**Transaction Throughput**:
```
Begins: 10000
Commits: 9950
Aborts: 50
Commit rate: 99.5%
```

## Troubleshooting

### Problem: Slow Queries

**Diagnosis**:
```bash
db_stat -m  # Check cache hit rate
```

**Solution**: Increase cache size if hit rate < 95%

### Problem: Lock Timeouts

**Error**: `DB_LOCK_DEADLOCK`

**Solution**:
1. Retry transaction
2. Reduce transaction duration
3. Reorder operations to avoid deadlock

### Problem: Database Corruption

**Symptoms**: Checksum errors, crashes

**Recovery**:
```bash
# 1. Verify database
db_verify database.db

# 2. Recover from backup + logs
cp backup/* .
db_recover -h .

# 3. If verification fails, dump/reload
db_dump -p database.db > dump.txt
# Fix manually if needed
db_load -T -f dump.txt new_database.db
```

### Problem: Slow Startup

**Cause**: Large transaction logs

**Solution**: Run checkpoint before shutdown
```c
env->txn_checkpoint(env, 0, 0, DB_FORCE);
```

## Best Practices

1. **Always use transactions** for consistency
2. **Batch operations** in single transaction for performance
3. **Monitor cache hit rate** and adjust cache size
4. **Regular checkpoints** reduce recovery time
5. **Archive logs** for point-in-time recovery
6. **Test restore procedures** before you need them
7. **Create indexes after bulk load** for speed
8. **Use read replicas** to scale read workload

## Comparison: SQLite vs BerkeleyDB

| Feature | SQLite Btree | BerkeleyDB |
|---------|--------------|------------|
| File format | Single file | Multiple files |
| Concurrency | Single writer, multi-reader | Single writer, multi-reader |
| MVCC | Yes (WAL mode) | Yes (always) |
| Replication | No | Yes (built-in) |
| Hot backup | Requires WAL mode | Yes (always) |
| Cache | Shared cache | Configurable |
| License | Public domain | BSD-style |

**DBSQL Choice**: BerkeleyDB for replication and enterprise features.

## Related Documentation

- [Architecture Overview](../architecture/ARCHITECTURE.md)
- [API Reference](../api/C_API.md)
- [Performance Tuning](PERFORMANCE.md)
- [BerkeleyDB Documentation](https://docs.oracle.com/cd/E17076_05/html/index.html)
