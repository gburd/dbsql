# ADR-002: Berkeley DB Storage Manager Integration with SQLite 3.52.0

**Status**: Accepted

**Date**: March 2026

**Context Owner**: DBSQL Modernization Team

---

## Context

DBSQL's defining characteristic is its use of Berkeley DB (BDB) as the storage engine instead of SQLite's native B-tree implementation. As we modernize to SQLite 3.52.0, we must preserve this integration while adapting to the new SQLite Btree API.

### Current State: DBSQL Storage Manager (sm.c)

DBSQL wraps Berkeley DB 4.8 with a custom Storage Manager (SM) layer that provides:

- **Database lifecycle**: `__sm_create`, `__sm_close_db`, `__sm_init`
- **Transactions**: `__sm_begin_txn`, `__sm_commit_txn`, `__sm_abort_txn`, `__sm_checkpoint`
- **Table/index management**: `__sm_create_table`, `__sm_create_index`, `__sm_drop_table`, `__sm_clear_table`
- **Cursor operations**: `__sm_cursor`, `__sm_close_cursor`, `__sm_moveto`, `__sm_first`, `__sm_last`, `__sm_next`, `__sm_prev`
- **Data access**: `__sm_key`, `__sm_data`, `__sm_key_size`, `__sm_data_size`, `__sm_key_compare`
- **Write operations**: `__sm_insert`, `__sm_delete`
- **Metadata**: `__sm_set_format_version`, `__sm_get_format_version`, `__sm_set_schema_sig`, `__sm_get_schema_sig`

This interface maps cleanly to SQLite 2.8.x's B-tree API but has gaps when compared to SQLite 3.52.0.

### Target State: SQLite 3.52.0 Btree API

SQLite 3.52.0's B-tree API includes:

- **Enhanced transactions**: Two-phase commit, savepoints, statement journals, write flags, schema cookie validation
- **Unified table/index creation**: Single `sqlite3BtreeCreateTable()` with flags instead of separate calls
- **Integer key optimization**: Tables use native `i64` rowids, not byte-encoded keys
- **Index key comparison**: Uses `UnpackedRecord` for structured key comparison
- **Payload unification**: Key and data treated as single "payload" with unified access functions
- **Cursor stability**: `CursorHasMoved` and `CursorRestore` for detecting and recovering from cursor invalidation
- **Enhanced insert/delete**: Flags for optimization hints, position preservation, auxiliary deletes
- **Metadata cookies**: Generic 16-cookie system for various metadata (schema version, user version, etc.)

### The Integration Challenge

The DBSQL SM must present the SQLite 3.52.0 Btree interface while internally using Berkeley DB primitives. The key challenges are:

1. **Key encoding mismatch**: SQLite 3.52.0 uses native i64 rowids; BDB uses byte keys
2. **Comparison semantics**: SQLite uses `UnpackedRecord` with collations; BDB uses byte-wise comparison
3. **Transaction model**: SQLite's two-phase commit and savepoints must map to BDB's transaction system
4. **Cursor stability**: BDB cursors can be invalidated; SQLite expects stable cursors with detection
5. **Payload unification**: SQLite treats key+data as one; BDB separates them

---

## Decision

We will create a **shim layer** (`__sm_btree_*` functions) that implements the SQLite 3.52.0 Btree API while internally calling the existing `__sm_*` functions.

This decision encompasses:

1. **Preserve the BDB integration**: Keep the existing `__sm_*` API unchanged
2. **Add 10 critical extensions**: Implement essential new SM functions
3. **Implement shim layer**: Create adapter functions that translate between SQLite and BDB conventions
4. **Handle key encoding at shim level**: Translate between i64 rowids and BDB byte keys
5. **Perform comparison in VDBE layer**: Use `UnpackedRecord` comparison in VDBE, then call BDB with raw keys

---

## Rationale

### Why Preserve BDB Integration?

1. **DBSQL's value proposition**: BDB provides features SQLite doesn't:
   - **Replication**: Multi-master and read-only replicas for high availability
   - **Crash recovery**: Industry-proven recovery subsystem
   - **Concurrent access**: Better multi-process/multi-thread scalability
   - **Production maturity**: 25+ years of deployment in critical systems

2. **Existing deployments**: DBSQL users rely on BDB-specific features. Switching to SQLite's B-tree would break backward compatibility.

3. **Engineering investment**: The BDB integration represents significant work. Discarding it would waste that investment.

### Why a Shim Layer?

We considered three approaches:

#### Option A: Rewrite SM to match SQLite 3.52.0 API exactly
**Rejected**: Would require deep changes to BDB integration, risking stability. BDB's transaction model and cursor semantics differ fundamentally from SQLite's B-tree.

#### Option B: Modify SQLite VDBE to call SM API directly
**Rejected**: Would fork SQLite's VDBE code, making future updates harder. Would lose SQLite's optimizations that assume B-tree semantics.

#### Option C: Create shim layer (CHOSEN)
**Accepted**: Minimal changes to both SM and VDBE. Isolates translation logic in one place. Allows independent testing of SM and VDBE.

### Alternative Storage Engines Considered

#### Native SQLite B-tree
**Rejected**: Loses BDB replication, crash recovery, and concurrency features. Would make DBSQL "just another SQLite fork" without differentiation.

#### LMDB (Lightning Memory-Mapped Database)
**Rejected**: Different transaction model, no replication, would require complete rewrite of storage layer.

#### RocksDB
**Rejected**: LSM-tree architecture has different performance characteristics (write-optimized vs. read-optimized). No replication. Would require complete rewrite.

---

## Consequences

### Positive

1. **Preserves BDB features**: Replication, crash recovery, concurrency remain available

2. **Backward compatibility**: Existing DBSQL databases continue to work (with migration for record format)

3. **Isolated changes**: Shim layer localizes translation logic, making bugs easier to fix

4. **Independent testing**: SM and VDBE can be tested separately

5. **Future flexibility**: If BDB needs to be replaced later, only the shim layer changes

### Negative

1. **Performance overhead**: Key encoding/decoding adds CPU cost (estimated 5-10% overhead)

2. **Complexity**: Shim layer adds another abstraction layer to understand and maintain

3. **Feature gaps**: Some SQLite 3.52.0 optimizations assume B-tree internals and won't work with BDB

4. **Debugging difficulty**: Issues may be in SM, shim, or VDBE; requires understanding all three

### Trade-offs vs. Native SQLite B-tree

| Aspect | DBSQL (BDB) | SQLite (Native B-tree) |
|--------|-------------|------------------------|
| Replication | Built-in (multi-master) | None (requires external tools) |
| Crash recovery | BDB's proven system | SQLite's write-ahead log |
| Concurrent readers | Excellent (MVCC) | Good (WAL mode) |
| Concurrent writers | Multiple processes | Single writer (queue) |
| Cursor stability | Manual detection needed | Automatic |
| Page cache | BDB manages | SQLite manages |
| File format | BDB B-tree | SQLite B-tree |
| Vacuuming | Auto-compact | Manual VACUUM |
| Performance (read) | ~10% slower (shim overhead) | Baseline |
| Performance (write) | ~5% slower (shim overhead) | Baseline |
| Maturity | 25+ years (BDB) | 20+ years (SQLite) |
| Deployment size | Larger (BDB lib) | Smaller (integrated) |

---

## Implementation: 10 Critical SM Extensions

These functions are **essential** for SQLite 3.52.0 compatibility:

### 1. `__sm_begin_txn_ex(sm, wrFlag, schemaCookie*)`
**Purpose**: Enhanced transaction begin with write flag and schema cookie return

**BDB mapping**: Call `DB_TXN->begin()`, then read schema cookie from metadata store

**Rationale**: SQLite needs to validate schema hasn't changed during transaction

### 2. `__sm_table_moveto(cursor, i64Key, bias, result*)`
**Purpose**: Integer key seek without byte encoding (tables store i64 rowids directly)

**BDB mapping**: Convert i64 to byte key using `INT_TO_KEY()`, call `DBC->get()` with `DB_SET_RANGE`

**Rationale**: SQLite 3.52.0 tables use native i64 rowids for performance

### 3. `__sm_index_moveto(cursor, pIdxKey, result*)`
**Purpose**: Seek using UnpackedRecord for index comparison

**BDB mapping**: Extract raw key bytes from UnpackedRecord, call `DBC->get()`, then compare

**Rationale**: Index keys are composite and need collation-aware comparison

### 4. `__sm_payload(cursor, offset, amt, buf)`
**Purpose**: Unified key+data access

**BDB mapping**: Determine cursor type; call `__sm_key()` or `__sm_data()` based on offset

**Rationale**: SQLite treats record as single payload; BDB separates key and data

### 5. `__sm_payload_size(cursor)`
**Purpose**: Unified size query

**BDB mapping**: Return `__sm_key_size()` + `__sm_data_size()`

**Rationale**: SQLite needs total record size for memory allocation

### 6. `__sm_integer_key(cursor)`
**Purpose**: Return i64 rowid directly

**BDB mapping**: Call `__sm_key()`, then convert bytes using `KEY_TO_INT()`

**Rationale**: Avoids repeated encoding/decoding of rowids

### 7. `__sm_insert_ex(cursor, pPayload, flags, seekResult)`
**Purpose**: Enhanced insert with BtreePayload struct

**BDB mapping**: Extract key/data from pPayload, handle flags (append hint, zero-blob), call `DBC->put()`

**Rationale**: SQLite optimizations use flags to hint insertion strategy

### 8. `__sm_delete_ex(cursor, flags)`
**Purpose**: Enhanced delete with flags

**BDB mapping**: Handle `BTREE_SAVEPOSITION` (cursor invalidation), call `DBC->del()`

**Rationale**: SQLite needs control over cursor position after delete

### 9. `__sm_eof(cursor)`
**Purpose**: Explicit EOF check

**BDB mapping**: Track EOF state in cursor wrapper

**Rationale**: BDB signals EOF via return codes; SQLite expects boolean check

### 10. `__sm_create_btree(sm, pgno*, flags)`
**Purpose**: Unified table/index creation with flags

**BDB mapping**: Parse flags (`BTREE_INTKEY`, `BTREE_BLOBKEY`), call `__sm_create_table()` or `__sm_create_index()`

**Rationale**: SQLite uses single function; DBSQL has separate functions

---

## Additional Extensions (Important but not Critical)

### Cursor Stability (11-12)
11. `__sm_cursor_has_moved(cursor)` - Detect if cursor was invalidated
12. `__sm_cursor_restore(cursor, differentRow*)` - Restore invalidated cursor

**BDB mapping**: Generation counter on cursors; increment on table modifications

### Metadata (13-14)
13. `__sm_get_meta(sm, idx, val*)` - Generic meta cookie access (16 cookies)
14. `__sm_update_meta(sm, idx, val)` - Generic meta cookie update

**BDB mapping**: Array of 16 cookies stored in metadata DB

### Enhanced Operations (15-18)
15. `__sm_clear_table_ex(sm, id, nChange*)` - Clear with change count
16. `__sm_drop_table_ex(sm, id, movedPage*)` - Drop with moved page info
17. `__sm_txn_state(sm)` - Query transaction state
18. `__sm_row_count_est(cursor)` - Estimated row count for query planner

**BDB mapping**: Leverage BDB's statistics APIs

---

## Key Encoding Strategy

SQLite 3.52.0 tables use native `i64` rowids, but BDB requires byte keys.

### Current DBSQL Encoding (SQLite 2.8.x)

4-byte big-endian with sign-bit flip:
```c
#define INT_TO_KEY(X, KEY) { \
  u_int32_t tmp = (u_int32_t)(X); \
  tmp ^= 0x80000000; \
  KEY[0] = (tmp >> 24) & 0xff; \
  KEY[1] = (tmp >> 16) & 0xff; \
  KEY[2] = (tmp >> 8) & 0xff; \
  KEY[3] = tmp & 0xff; \
}
```

### New Encoding (SQLite 3.52.0)

8-byte big-endian with sign-bit flip:
```c
#define I64_TO_KEY(X, KEY) { \
  u_int64_t tmp = (u_int64_t)(X); \
  tmp ^= 0x8000000000000000ULL; \
  KEY[0] = (tmp >> 56) & 0xff; \
  KEY[1] = (tmp >> 48) & 0xff; \
  KEY[2] = (tmp >> 40) & 0xff; \
  KEY[3] = (tmp >> 32) & 0xff; \
  KEY[4] = (tmp >> 24) & 0xff; \
  KEY[5] = (tmp >> 16) & 0xff; \
  KEY[6] = (tmp >> 8) & 0xff; \
  KEY[7] = tmp & 0xff; \
}
```

**Migration impact**: Existing DBSQL databases must convert all table keys from 4-byte to 8-byte format.

---

## Index Comparison Strategy

SQLite 3.52.0 index keys use `UnpackedRecord` with collation-aware comparison.

### Approach: Hybrid Comparison

1. **VDBE performs comparison**: When comparing index keys, VDBE unpacks both keys into `UnpackedRecord` structs and compares them using SQLite's collation functions.

2. **SM performs seek**: VDBE converts the target key to raw bytes, then calls `__sm_index_moveto()` to position the cursor.

3. **VDBE validates result**: After seek, VDBE re-compares the cursor's key to the target to determine if it's an exact match.

**Rationale**: BDB's byte-wise comparison doesn't understand SQLite collations (NOCASE, RTRIM, ICU, etc.). Pushing comparison into VDBE preserves correct semantics.

**Performance impact**: Seek operations require double comparison (once to seek, once to validate). Estimated 10-15% overhead on index scans.

---

## Cursor Stability Handling

SQLite 3.52.0 expects cursors to remain valid unless explicitly detected as moved.

### BDB Cursor Invalidation

BDB invalidates cursors when:
- The underlying record is deleted
- The B-tree is compacted
- The database is closed and reopened

### Detection Strategy

Wrap BDB cursors with generation counter:
```c
typedef struct sm_cursor_wrapper {
  DBC *dbc;              /* BDB cursor */
  u32 generation;        /* Generation when cursor created */
  u32 table_generation;  /* Current table generation */
} sm_cursor_wrapper_t;
```

When table is modified, increment `table_generation`. Before cursor operations, check if `generation != table_generation`. If mismatch, cursor is invalid.

### Restoration Strategy

If cursor is invalid, re-seek to last known position:
```c
int __sm_cursor_restore(sm_cursor_t *cursor, int *differentRow) {
  if (cursor->generation == cursor->table_generation) {
    *differentRow = 0;
    return DBSQL_SUCCESS;  /* cursor still valid */
  }

  /* Re-seek to last key */
  int rc = __sm_moveto(cursor, cursor->lastKey, cursor->lastKeyLen, &res);
  *differentRow = (res != 0);  /* 0 = exact match, else different row */
  cursor->generation = cursor->table_generation;
  return rc;
}
```

---

## Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Key encoding bugs | MEDIUM | HIGH | Extensive unit tests for encoding/decoding; property-based testing |
| Cursor invalidation missed | MEDIUM | HIGH | Generation counter on every cursor; test with concurrent modifications |
| Performance regression | HIGH | MEDIUM | Benchmark each shim function; profile hot paths; optimize critical paths |
| Comparison semantic mismatch | LOW | CRITICAL | Comprehensive collation test suite; validate against SQLite reference |
| Transaction model mismatch | MEDIUM | HIGH | Map SQLite transaction states to BDB states; test all state transitions |

---

## Testing Strategy

### Unit Tests (sm_test.c)

Test each SM function in isolation:
- Key encoding/decoding correctness
- Cursor seek correctness
- Transaction state transitions
- Metadata storage/retrieval

### Integration Tests (vdbe_sm_test.c)

Test VDBE calling SM through shim layer:
- Basic CRUD operations
- Multi-cursor scenarios
- Transaction boundaries
- Error handling

### Compatibility Tests (sqlite_test_suite.c)

Run SQLite's test suite (millions of tests) against DBSQL SM:
- All standard SQL operations
- Edge cases and error conditions
- Stress tests

### Performance Tests (benchmark.c)

Measure overhead of shim layer:
- Insert/select/update/delete microbenchmarks
- TPC-C style workload
- Comparison with native SQLite B-tree

---

## References

- [VDBE Analysis Report](/home/gburd/ws/dbsql/vdbe_analysis_report.md) - SM API gap analysis (Section 3)
- Berkeley DB Reference Manual (Oracle) - BDB transaction and cursor APIs
- SQLite 3.52.0 Source Code (`src/btree.c`, `src/btreeInt.h`) - Btree API reference
- DBSQL Storage Manager (`src/sm.c`) - Current implementation

---

## Decision Log

- **2026-03-07**: SM gap analysis completed; 10 critical extensions identified
- **2026-03-16**: ADR-002 written and accepted; shim layer approach chosen
