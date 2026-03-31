# ALTER TABLE Architecture

## Component Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     SQL Input String                         │
│        "ALTER TABLE users RENAME TO people"                  │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                   Tokenizer                                  │
│              (src/sql_tokenize.c)                           │
│                                                              │
│  Breaks SQL into tokens:                                     │
│  [TK_ALTER] [TK_TABLE] [TK_ID:"users"]                      │
│  [TK_RENAME] [TK_TO] [TK_ID:"people"]                       │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                   Parser (Lemon)                             │
│              (src/sql_parser.y → sql_parser.c)              │
│                                                              │
│  Grammar Rule Matched:                                       │
│  cmd ::= ALTER TABLE fullname(X) RENAME TO nm(Y).          │
│          {__alter_rename_table(pParse,X,&Y);}              │
│                                                              │
│  Calls action routine with parsed components                │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              ALTER TABLE Functions                           │
│                (src/cg_alter.c)                             │
│                                                              │
│  __alter_rename_table(parser, src_list, new_name)          │
│  ├─ Validate table exists                                   │
│  ├─ Check not system table                                  │
│  ├─ Check not read-only                                     │
│  ├─ Verify new name available                               │
│  └─ Generate VDBE opcodes                                   │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              VDBE Code Generation                            │
│                (Virtual Machine Bytecode)                    │
│                                                              │
│  Generated Opcodes:                                          │
│  OP_Transaction  0 1 0     (Begin write txn)                │
│  OP_Integer     0 1 0      (Database index)                 │
│  OP_OpenWrite   0 0 2      (Open DBSQL_MASTER)             │
│  OP_String8     0 2 0 "people"  (New name)                 │
│  OP_String8     0 3 0 "users"   (Old name)                 │
│  OP_Rewind      0 7 0      (Start of MASTER scan)          │
│  OP_Column      0 2 4      (Read name column)               │
│  OP_Eq          3 4 0      (Compare with old name)          │
│  OP_Next        0 5 0      (Next record)                    │
│  OP_Column      0 0 5      (Read type)                      │
│  OP_MakeRecord  2 5 6      (Build new record)               │
│  OP_Delete      0 0 0      (Delete old record)              │
│  OP_Insert      0 6 0      (Insert new record)              │
│  OP_Gosub       8 0 0      (Schema change notification)     │
│  OP_Commit      0 0 0      (Commit transaction)             │
│  OP_Halt        0 0 0      (Done)                           │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              VDBE Execution Engine                           │
│                (src/vdbe.c)                                 │
│                                                              │
│  Executes bytecode:                                          │
│  1. Starts transaction                                       │
│  2. Opens DBSQL_MASTER table                                │
│  3. Finds record with old name                               │
│  4. Updates to new name                                      │
│  5. Marks schema changed                                     │
│  6. Commits transaction                                      │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              Storage Manager (SM)                            │
│                (src/sm.c)                                   │
│                                                              │
│  Berkeley DB Operations:                                     │
│  - Open DBSQL_MASTER database                               │
│  - Cursor operations (scan, update, delete, insert)         │
│  - Transaction management                                    │
│  - Schema signature updates                                  │
│  - File rename (for RENAME TABLE)                           │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              Berkeley DB                                     │
│                                                              │
│  Physical Storage:                                           │
│  - main.db (primary database)                                │
│    ├─ __DBSQL_main_meta__ (schema metadata)                │
│    ├─ main_tbl.0000000002 (DBSQL_MASTER)                   │
│    ├─ main_tbl.0000000003 (users table) → RENAMED          │
│    └─ main_tbl.0000000003 (people table)                   │
└─────────────────────────────────────────────────────────────┘
```

## Data Flow Diagrams

### 1. RENAME TABLE Flow

```
SQL: ALTER TABLE users RENAME TO people

┌─────────┐    ┌─────────┐    ┌──────────┐    ┌────────┐
│ Parser  │───▶│  Alter  │───▶│   VDBE   │───▶│   SM   │
│         │    │ Rename  │    │   Code   │    │        │
└─────────┘    └─────────┘    └──────────┘    └────────┘
                   │                               │
                   ├─ Validate table              │
                   ├─ Check permissions           │
                   ├─ Verify new name             │
                   │                               │
                   └─ Generate opcodes ──────────▶│
                                                   │
                                          ┌────────┴────────┐
                                          │ Update metadata │
                                          │ Rename DB files │
                                          │ Bump schema ver │
                                          └─────────────────┘
```

### 2. ADD COLUMN Flow

```
SQL: ALTER TABLE users ADD COLUMN email TEXT DEFAULT 'none'

┌─────────┐    ┌─────────┐    ┌──────────┐
│ Parser  │───▶│  Alter  │───▶│   VDBE   │
│         │    │  Add    │    │   Code   │
└─────────┘    │ Column  │    └──────────┘
               └────┬────┘         │
                    │              │
     ┌──────────────┘              │
     │                             │
     ├─ Parse column def          │
     ├─ Validate constraints       │
     ├─ Add to table schema       │
     │                             │
     └─ Generate VDBE ────────────┤
                                   │
                          ┌────────┴────────┐
                          │ Extend schema   │
                          │ Apply DEFAULT   │
                          │ Update rows     │
                          └─────────────────┘
```

### 3. DROP COLUMN Flow (Table Rebuild)

```
SQL: ALTER TABLE users DROP COLUMN email

┌─────────┐    ┌─────────┐    ┌──────────┐
│ Parser  │───▶│  Alter  │───▶│ Multiple │
│         │    │  Drop   │    │   VDBE   │
└─────────┘    │ Column  │    │Programs  │
               └────┬────┘    └──────────┘
                    │              │
     ┌──────────────┘              │
     │                             │
     ├─ Validate column exists     │
     ├─ Check not last column     │
     ├─ Check not primary key     │
     │                             │
     └─ Generate rebuild plan ────┤
                                   │
                    ┌──────────────┴──────────────┐
                    │ 1. CREATE TABLE tmp AS      │
                    │    SELECT (all but col)     │
                    │ 2. DROP TABLE old           │
                    │ 3. ALTER TABLE tmp          │
                    │    RENAME TO old            │
                    │ 4. Rebuild indexes          │
                    └─────────────────────────────┘
```

## Function Call Hierarchy

```
dbsql_exec()
 └─ __run_parser()
     └─ sql_parser()  [generated by lemon]
         └─ __alter_rename_table()  [src/cg_alter.c]
             ├─ __src_list_lookup()  [validate]
             ├─ __schema_to_index()  [get DB index]
             ├─ __is_table_read_only()  [check perms]
             ├─ __find_table()  [check new name]
             └─ __parser_get_vdbe()
                 ├─ __vdbe_begin_write_operation()
                 ├─ __open_master_table()
                 ├─ __vdbe_add_op*()  [generate opcodes]
                 ├─ __change_schema_signature()
                 └─ __vdbe_conclude_write()
                     └─ __vdbe_exec()
                         └─ SM operations
                             ├─ __sm_cursor()
                             ├─ __sm_moveto()
                             ├─ __sm_delete()
                             ├─ __sm_insert()
                             └─ __sm_commit_txn()
```

## Schema Evolution Process

```
┌─────────────────────────────────────────────────┐
│           Initial Schema State                   │
├─────────────────────────────────────────────────┤
│ DBSQL_MASTER:                                   │
│   type='table', name='users',                   │
│   tbl_name='users', rootpage=3,                 │
│   sql='CREATE TABLE users(id, name)'           │
│                                                  │
│ Schema Cookie: 1                                 │
└─────────────────────────────────────────────────┘
                       │
                       │ ALTER TABLE users
                       │ RENAME TO people
                       ▼
┌─────────────────────────────────────────────────┐
│            After ALTER TABLE                     │
├─────────────────────────────────────────────────┤
│ DBSQL_MASTER:                                   │
│   type='table', name='people',                  │
│   tbl_name='people', rootpage=3,                │
│   sql='CREATE TABLE people(id, name)'          │
│                                                  │
│ Schema Cookie: 2  ← INCREMENTED                 │
└─────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────┐
│         Schema Reload Triggered                  │
├─────────────────────────────────────────────────┤
│ Next statement execution detects cookie         │
│ change and reloads schema from DBSQL_MASTER     │
└─────────────────────────────────────────────────┘
```

## Error Handling Flow

```
SQL: ALTER TABLE nonexistent RENAME TO foo

┌─────────┐
│ Parser  │
└────┬────┘
     │
     ▼
┌──────────────────┐
│  Alter Rename    │
│  ├─ Lookup table │───┐
│  │   FAILED!     │   │
│  └───────────────┘   │
         │              │
         ▼              │
┌──────────────────┐   │
│ Error Handling   │◀──┘
│ ├─ Set parser->  │
│ │   zErrMsg     │
│ └─ Increment     │
│     parser->nErr │
└────┬─────────────┘
     │
     ▼
┌──────────────────┐
│ Return to caller │
│ with error       │
└──────────────────┘
```

## Memory Management

```
┌────────────────────────────────────────────┐
│         Memory Allocation Pattern           │
├────────────────────────────────────────────┤
│                                             │
│  Token Copies:                              │
│  z_name = __table_name_from_token(&token)  │
│         ↓                                   │
│    __dbsql_calloc() or __dbsql_strdup()    │
│                                             │
│  VDBE Strings:                              │
│  __vdbe_add_op4(v, ..., "string", 0)       │
│         ↓                                   │
│    VDBE takes ownership                     │
│    (freed in __vdbe_delete())              │
│                                             │
│  src_list cleanup:                          │
│  __src_list_delete(src)                    │
│         ↓                                   │
│    Frees all tokens and structures          │
│                                             │
└────────────────────────────────────────────┘
```

## Transaction Boundaries

```
┌────────────────────────────────────────┐
│      Transaction Lifecycle             │
├────────────────────────────────────────┤
│                                         │
│  1. __vdbe_begin_write_operation()     │
│     ├─ Generates OP_Transaction        │
│     └─ Sets parser->writeMask          │
│                                         │
│  2. ... ALTER TABLE operations ...     │
│     ├─ DBSQL_MASTER modifications      │
│     ├─ Index updates                   │
│     └─ Schema signature bump           │
│                                         │
│  3. __vdbe_conclude_write()            │
│     └─ Generates OP_Commit             │
│                                         │
│  On Error:                              │
│     Automatic rollback via VDBE        │
│     cleanup (no OP_Commit issued)      │
│                                         │
└────────────────────────────────────────┘
```

## Concurrency Model

```
┌────────────────────────────────────────────┐
│         ALTER TABLE Locking                 │
├────────────────────────────────────────────┤
│                                             │
│  Schema Lock Hierarchy:                     │
│  ┌─────────────────────────────┐           │
│  │ 1. Database-level lock      │           │
│  │    (Berkeley DB txn)        │           │
│  │         │                   │           │
│  │         ▼                   │           │
│  │ 2. DBSQL_MASTER write lock │           │
│  │    (exclusive)              │           │
│  │         │                   │           │
│  │         ▼                   │           │
│  │ 3. Schema change flag       │           │
│  │    (schema_cookie++)        │           │
│  │         │                   │           │
│  │         ▼                   │           │
│  │ 4. Other connections detect │           │
│  │    change and reload schema │           │
│  └─────────────────────────────┘           │
│                                             │
│  No row-level locks needed for ALTER TABLE │
│  (metadata-only operation in most cases)   │
│                                             │
└────────────────────────────────────────────┘
```

## Integration Points

### 1. Parser Integration
- `src/sql_parser.y`: Grammar rules
- Token definitions flow from sql_tokenize.c

### 2. Code Generation
- `src/cg_alter.c`: Generates VDBE opcodes
- Uses common patterns from cg_build.c

### 3. VDBE Execution
- `src/vdbe.c`: Executes generated opcodes
- No ALTER-specific opcodes needed

### 4. Storage Manager
- `src/sm.c`: Berkeley DB operations
- May need __sm_rename_table() addition

### 5. Schema Management
- `src/cg_build.c`: Schema utilities
- __change_schema_signature() triggers reload

## File Organization

```
src/
├── cg_alter.c         ← NEW: ALTER TABLE implementation
├── cg_build.c         ← Reference for table operations
├── sql_parser.y       ← Modified: Grammar rules
├── sql_tokenize.c     ← Unchanged: Has keywords
├── sm.c               ← May need additions
└── inc/
    ├── dbsql_ext.h    ← Modified: Function declarations
    └── sql_tokens.h   ← Unchanged: Token definitions

test/src/
└── test_alter_table.c ← NEW: Test suite

docs/
├── ALTER_TABLE_ARCHITECTURE.md    ← This file
├── ALTER_TABLE_IMPLEMENTATION.md  ← Technical details
├── ALTER_TABLE_SUMMARY.md         ← Implementation summary
└── ALTER_TABLE_QUICKSTART.md      ← Quick reference
```

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| RENAME TABLE | O(1) | Metadata update only |
| ADD COLUMN | O(n) | If DEFAULT specified, updates all rows |
| RENAME COLUMN | O(1) | Metadata update + SQL rewrite |
| DROP COLUMN | O(n) | Requires table rebuild |

Where n = number of rows in table.

## Future Enhancements

### Online Schema Changes
```
Traditional:         Online:
┌──────┐            ┌──────┐
│ Lock │            │ Copy │
│ Table│            │ Table│◄─── Background
│      │            │      │
│Modify│            │Swap  │
│Schema│            │Ptrs  │◄─── Milliseconds
│      │            │      │
│Unlock│            │Done  │
└──────┘            └──────┘
  Hours               Fast
```

### Atomic Multi-ALTER
```sql
ALTER TABLE users
  ADD COLUMN email TEXT,
  ADD COLUMN phone TEXT,
  DROP COLUMN temp_field;

-- Single transaction, single schema version bump
```

## Debugging Tips

1. **Enable VDBE Tracing**:
   ```c
   #define VDBE_PROFILE 1
   __vdbe_trace(v, stdout);
   ```

2. **Check Schema Cookie**:
   ```sql
   PRAGMA schema_version;
   ```

3. **Dump DBSQL_MASTER**:
   ```sql
   SELECT * FROM dbsql_master WHERE type='table';
   ```

4. **VDBE Listing**:
   ```c
   __vdbe_list(v);
   ```

---

**Last Updated:** 2024-03-31
**Diagram Format:** ASCII Art (for text-based viewing)
**Related Docs:** See ALTER_TABLE_IMPLEMENTATION.md for code details
