# DBSQL TRIGGER Implementation Specification

**Task**: #140 - Implement TRIGGER support
**Estimated Effort**: 40-80 hours
**Priority**: HIGH (Production requirement)
**Status**: PENDING (Will start after tasks #133-139 complete)

---

## 📋 OVERVIEW

Implement SQL TRIGGER functionality in DBSQL to support:
- BEFORE/AFTER triggers
- INSERT/UPDATE/DELETE events
- FOR EACH ROW execution
- NEW/OLD row references
- Trigger condition (WHEN clause)

---

## 🎯 REQUIREMENTS

### SQL Syntax to Support

```sql
-- Create trigger
CREATE TRIGGER [IF NOT EXISTS] trigger_name
  BEFORE|AFTER INSERT|UPDATE|DELETE ON table_name
  [FOR EACH ROW]
  [WHEN condition]
BEGIN
  sql_statement;
  [sql_statement;] ...
END;

-- Drop trigger
DROP TRIGGER [IF EXISTS] trigger_name;

-- List triggers
SELECT name FROM dbsql_master WHERE type='trigger';
PRAGMA trigger_list(table_name);
```

### Trigger Examples

**Example 1: Audit trigger**
```sql
CREATE TRIGGER audit_users
  AFTER UPDATE ON users
  FOR EACH ROW
BEGIN
  INSERT INTO audit_log VALUES(OLD.id, OLD.email, NEW.email, datetime('now'));
END;
```

**Example 2: Validation trigger**
```sql
CREATE TRIGGER validate_email
  BEFORE INSERT ON users
  FOR EACH ROW
  WHEN NEW.email NOT LIKE '%@%'
BEGIN
  SELECT RAISE(ABORT, 'Invalid email address');
END;
```

**Example 3: Cascading trigger**
```sql
CREATE TRIGGER delete_user_posts
  AFTER DELETE ON users
  FOR EACH ROW
BEGIN
  DELETE FROM posts WHERE user_id = OLD.id;
END;
```

---

## 🏗️ ARCHITECTURE

### Parser Changes (sql_parser.y)

**Add tokens**:
```c
%token TK_TRIGGER       // TRIGGER keyword (may already exist)
%token TK_BEFORE        // BEFORE keyword
%token TK_AFTER         // AFTER keyword
%token TK_INSTEAD       // INSTEAD OF (for views)
%token TK_EACH          // EACH keyword
%token TK_ROW           // ROW keyword
%token TK_RAISE         // RAISE function
```

**Add grammar rules**:
```yacc
// CREATE TRIGGER statement
cmd ::= CREATE trigger_opt TRIGGER ifnotexists nm ON nm
        trigger_time trigger_event FOR EACH ROW trigger_when
        BEGIN trigger_cmd_list END(semi).
{
    __dbsql_create_trigger(pParse, &n1, &n2, time, event, when, cmd_list);
}

trigger_opt ::= TEMP|TEMPORARY.  // Optional
trigger_opt ::= .

trigger_time ::= BEFORE.         { $$ = TK_BEFORE; }
trigger_time ::= AFTER.          { $$ = TK_AFTER; }
trigger_time ::= INSTEAD OF.     { $$ = TK_INSTEAD; }

trigger_event ::= DELETE.        { $$ = TK_DELETE; }
trigger_event ::= INSERT.        { $$ = TK_INSERT; }
trigger_event ::= UPDATE.        { $$ = TK_UPDATE; }
trigger_event ::= UPDATE OF nm_list.  // Column-specific

trigger_when ::= .               { $$ = NULL; }
trigger_when ::= WHEN expr.      { $$ = expr; }

trigger_cmd_list ::= trigger_cmd_list trigger_cmd SEMI.
trigger_cmd_list ::= trigger_cmd SEMI.

trigger_cmd ::= select_stmt.     // INSERT/UPDATE/DELETE/SELECT
trigger_cmd ::= RAISE LP IGNORE RP.
trigger_cmd ::= RAISE LP ROLLBACK COMMA STRING RP.
trigger_cmd ::= RAISE LP ABORT COMMA STRING RP.
trigger_cmd ::= RAISE LP FAIL COMMA STRING RP.

// DROP TRIGGER statement
cmd ::= DROP TRIGGER ifexists nm.
{
    __dbsql_drop_trigger(pParse, &nm, exists);
}
```

### Data Structures (dbsql_int.h)

**New trigger_t structure**:
```c
typedef struct trigger trigger_t;
typedef struct trigger_step trigger_step_t;

struct trigger {
    char *name;                  // Trigger name
    char *table_name;            // Table name
    u8 op;                       // TK_INSERT, TK_UPDATE, TK_DELETE
    u8 time;                     // TK_BEFORE, TK_AFTER, TK_INSTEAD
    expr_t *when_clause;         // WHEN condition (may be NULL)
    trigger_step_t *step_list;   // Trigger body (linked list)
    trigger_t *next_trigger;     // Next trigger on same table
};

struct trigger_step {
    int op;                      // TK_INSERT, TK_UPDATE, TK_DELETE, TK_SELECT
    int orconf;                  // OR ROLLBACK, OR ABORT, etc.
    select_t *select_stmt;       // For INSERT/UPDATE/DELETE
    char *target_table;          // Target table name
    expr_t *where_clause;        // WHERE clause (for UPDATE/DELETE)
    expr_list_t *expr_list;      // SET clause (for UPDATE)
    id_list_t *column_list;      // Column list (for INSERT)
    trigger_step_t *next_step;   // Next step in trigger body
};
```

**Add to table_t structure**:
```c
struct table {
    // ... existing fields ...
    trigger_t *first_trigger;    // Linked list of triggers
};
```

**Add to parser_t structure**:
```c
struct parser {
    // ... existing fields ...
    trigger_t *new_trigger;      // Trigger being created
    int trigger_stack;           // Recursion depth (prevent infinite)
};
```

### Code Generator (new file: cg_trigger.c)

**Key functions to implement**:

```c
// Create trigger
void __dbsql_create_trigger(parser_t *parser, token_t *name1,
                             token_t *name2, int time, int event,
                             expr_t *when, trigger_step_t *steps);

// Drop trigger
void __dbsql_drop_trigger(parser_t *parser, token_t *name, int if_exists);

// Execute trigger (called from INSERT/UPDATE/DELETE)
int __dbsql_execute_trigger(parser_t *parser, trigger_t *trigger,
                             int op, table_t *table, int reg_old, int reg_new);

// Code trigger steps
void __dbsql_code_trigger_steps(parser_t *parser, trigger_step_t *steps,
                                int reg_old, int reg_new, int orconf);

// Build trigger step (called during parsing)
trigger_step_t *__dbsql_trigger_step(parser_t *parser, int op,
                                     token_t *table, select_t *select,
                                     expr_t *where, expr_list_t *expr_list,
                                     id_list_t *column_list);

// Attach trigger to table
void __dbsql_trigger_attach(parser_t *parser, trigger_t *trigger);

// Detach all triggers from table
void __dbsql_trigger_detach(table_t *table);

// Check trigger recursion
int __dbsql_trigger_check_recursion(parser_t *parser, trigger_t *trigger);
```

### VDBE Opcodes

**No new opcodes required** - triggers execute as inline VDBE code
- Trigger VDBE code is generated and inserted at trigger points
- Use existing opcodes: OP_Program, OP_Param, OP_Gosub, OP_Return

**Pseudo-opcodes for trigger context**:
- Use registers for OLD and NEW pseudo-tables
- OLD register contains pre-modification values
- NEW register contains post-modification values

### Storage Manager (sm.c)

**Trigger metadata storage**:
```c
// Store trigger definition in dbsql_master table
INSERT INTO dbsql_master (type, name, tbl_name, rootpage, sql)
VALUES ('trigger', 'trigger_name', 'table_name', 0, 'CREATE TRIGGER ...');
```

**Load triggers at startup**:
```c
// Query all triggers for a table
SELECT sql FROM dbsql_master WHERE type='trigger' AND tbl_name='table_name';

// Parse and attach triggers to table structure
```

---

## 🔧 IMPLEMENTATION STEPS

### Phase 1: Parser & Data Structures (8-12 hours)

1. **Add tokens** to sql_tokenize.c
   - TK_TRIGGER, TK_BEFORE, TK_AFTER, TK_INSTEAD, TK_EACH, TK_ROW, TK_RAISE

2. **Add grammar rules** to sql_parser.y
   - CREATE TRIGGER syntax
   - DROP TRIGGER syntax
   - Trigger timing, events, conditions, steps

3. **Define structures** in dbsql_int.h
   - trigger_t
   - trigger_step_t
   - Add fields to table_t and parser_t

### Phase 2: Code Generation (16-24 hours)

4. **Create cg_trigger.c**
   - Implement __dbsql_create_trigger()
   - Implement __dbsql_drop_trigger()
   - Implement __dbsql_execute_trigger()
   - Implement __dbsql_code_trigger_steps()

5. **Modify existing code generators**
   - **cg_insert.c**: Call __dbsql_execute_trigger() for INSERT triggers
   - **cg_update.c**: Call __dbsql_execute_trigger() for UPDATE triggers
   - **cg_delete.c**: Call __dbsql_execute_trigger() for DELETE triggers

6. **Handle NEW/OLD references**
   - Modify expression evaluator to recognize NEW.column and OLD.column
   - Map to appropriate registers
   - **cg_expr.c**: Add TK_TRIGGER handling

### Phase 3: Metadata & Storage (8-12 hours)

7. **Trigger persistence**
   - Store trigger SQL in dbsql_master table
   - Load triggers at database open
   - Attach triggers to table structures

8. **Trigger introspection**
   - Implement PRAGMA trigger_list(table)
   - Query dbsql_master for triggers

### Phase 4: Validation & Safety (8-12 hours)

9. **Recursion prevention**
   - Track trigger stack depth
   - Prevent infinite trigger loops
   - Max depth: 10 (configurable)

10. **Constraint checks**
    - Validate trigger names (unique per database)
    - Verify table exists
    - Check column names in UPDATE OF

11. **Error handling**
    - RAISE(ABORT, message)
    - RAISE(FAIL, message)
    - RAISE(ROLLBACK, message)
    - RAISE(IGNORE) - skip current row

### Phase 5: Testing (16-24 hours)

12. **Unit tests** (test/src/test_triggers.c)
    - CREATE/DROP TRIGGER
    - BEFORE/AFTER triggers
    - INSERT/UPDATE/DELETE events
    - NEW/OLD references
    - WHEN conditions
    - RAISE functions
    - Trigger recursion

13. **Integration tests**
    - Audit logging triggers
    - Validation triggers
    - Cascading deletes
    - Complex multi-step triggers

14. **Edge case tests**
    - Trigger on non-existent table
    - Infinite recursion detection
    - Transaction rollback with triggers
    - Multiple triggers on same event

---

## 🧪 TEST CASES

### Test 1: Basic AFTER INSERT Trigger
```sql
CREATE TABLE log(id INTEGER, msg TEXT);
CREATE TABLE users(id INTEGER, name TEXT);

CREATE TRIGGER log_insert
  AFTER INSERT ON users
  FOR EACH ROW
BEGIN
  INSERT INTO log VALUES(NEW.id, 'User created: ' || NEW.name);
END;

INSERT INTO users VALUES(1, 'Alice');
SELECT * FROM log;  -- Expected: 1|User created: Alice
```

### Test 2: BEFORE UPDATE Validation
```sql
CREATE TRIGGER validate_age
  BEFORE UPDATE OF age ON users
  FOR EACH ROW
  WHEN NEW.age < 0
BEGIN
  SELECT RAISE(ABORT, 'Age cannot be negative');
END;

UPDATE users SET age=-5 WHERE id=1;
-- Expected: Error: Age cannot be negative
```

### Test 3: AFTER DELETE Cascade
```sql
CREATE TRIGGER cascade_delete
  AFTER DELETE ON users
  FOR EACH ROW
BEGIN
  DELETE FROM posts WHERE user_id = OLD.id;
  DELETE FROM comments WHERE user_id = OLD.id;
END;

DELETE FROM users WHERE id=1;
-- Expected: All posts and comments for user 1 deleted
```

### Test 4: Recursion Prevention
```sql
CREATE TRIGGER trigger1
  AFTER INSERT ON t1
  FOR EACH ROW
BEGIN
  INSERT INTO t2 VALUES(NEW.id);
END;

CREATE TRIGGER trigger2
  AFTER INSERT ON t2
  FOR EACH ROW
BEGIN
  INSERT INTO t1 VALUES(NEW.id + 1);  -- Would cause infinite loop
END;

INSERT INTO t1 VALUES(1);
-- Expected: Error after 10 iterations (max recursion depth)
```

---

## 🚧 KNOWN CHALLENGES

### Challenge 1: NEW/OLD Register Allocation
**Problem**: Triggers need access to row values before/after modification
**Solution**:
- Allocate registers for OLD row values before DELETE/UPDATE
- Allocate registers for NEW row values after INSERT/UPDATE
- Pass register numbers to trigger execution

### Challenge 2: Trigger Timing
**Problem**: BEFORE triggers must execute before the operation
**Solution**:
- Generate VDBE code in correct order:
  1. BEFORE triggers
  2. Main operation (INSERT/UPDATE/DELETE)
  3. AFTER triggers

### Challenge 3: Transaction Semantics
**Problem**: Trigger errors must rollback the triggering operation
**Solution**:
- RAISE(ABORT): Rollback current statement
- RAISE(ROLLBACK): Rollback entire transaction
- RAISE(FAIL): Return error but don't rollback
- RAISE(IGNORE): Skip row and continue

### Challenge 4: Recursion Detection
**Problem**: Triggers can trigger other triggers (infinite loops)
**Solution**:
- Track trigger execution stack in parser_t
- Limit recursion depth to 10 (configurable)
- Detect direct recursion (trigger triggers itself)

---

## 📝 STUB FUNCTIONS TO REPLACE

Currently in `src/stubs.c`:

```c
// These 10 functions need to be moved to cg_trigger.c and implemented:

void __dbsql_begin_trigger(...) { DBSQL_ASSERT(0); }
void __dbsql_finish_trigger(...) { DBSQL_ASSERT(0); }
void __dbsql_drop_trigger(...) { DBSQL_ASSERT(0); }
void __dbsql_drop_trigger_ptr(...) { DBSQL_ASSERT(0); }
trigger_step_t *__dbsql_trigger_select_step(...) { DBSQL_ASSERT(0); return NULL; }
trigger_step_t *__dbsql_trigger_insert_step(...) { DBSQL_ASSERT(0); return NULL; }
trigger_step_t *__dbsql_trigger_update_step(...) { DBSQL_ASSERT(0); return NULL; }
trigger_step_t *__dbsql_trigger_delete_step(...) { DBSQL_ASSERT(0); return NULL; }
void __dbsql_code_row_trigger(...) { DBSQL_ASSERT(0); }
int __dbsql_triggers_exist(...) { DBSQL_ASSERT(0); return 0; }
```

---

## 🎯 SUCCESS CRITERIA

### Code Quality
- ✅ All 10 stub functions implemented
- ✅ Zero compiler warnings
- ✅ All tests pass
- ✅ No memory leaks (valgrind clean)
- ✅ Documentation complete

### Functionality
- ✅ CREATE TRIGGER works for all timing/event combinations
- ✅ DROP TRIGGER works
- ✅ BEFORE/AFTER/INSTEAD OF triggers execute correctly
- ✅ INSERT/UPDATE/DELETE events handled
- ✅ NEW/OLD references work
- ✅ WHEN conditions evaluated correctly
- ✅ RAISE functions work (ABORT, ROLLBACK, FAIL, IGNORE)
- ✅ Recursion prevented (max depth 10)
- ✅ Trigger metadata persisted correctly
- ✅ PRAGMA trigger_list works

### Performance
- ✅ Minimal overhead when no triggers exist
- ✅ Efficient trigger execution (no unnecessary VDBE code)
- ✅ Reasonable recursion limit (not too low, not too high)

---

## 📚 REFERENCE MATERIALS

### SQLite Documentation
- https://www.sqlite.org/lang_createtrigger.html
- https://www.sqlite.org/lang_droptrigger.html
- https://www.sqlite.org/lang_explain.html (for VDBE inspection)

### SQLite Source Code (Reference)
- `src/trigger.c` - Main trigger implementation
- `src/parse.y` - Grammar rules for triggers
- `src/insert.c`, `src/update.c`, `src/delete.c` - Trigger invocation

### DBSQL Source Files to Modify
- `src/sql_parser.y` - Add grammar rules
- `src/sql_tokenize.c` - Add tokens
- `src/cg_trigger.c` - NEW FILE - Main implementation
- `src/cg_insert.c` - Add trigger invocation
- `src/cg_update.c` - Add trigger invocation
- `src/cg_delete.c` - Add trigger invocation
- `src/cg_expr.c` - Add NEW/OLD handling
- `src/inc/dbsql_int.h` - Add structures
- `src/stubs.c` - Remove 10 stub functions

---

## 📊 ESTIMATED BREAKDOWN

| Phase | Task | Hours | Complexity |
|-------|------|-------|------------|
| 1 | Parser & structures | 8-12 | Medium |
| 2 | Code generation | 16-24 | High |
| 3 | Metadata & storage | 8-12 | Medium |
| 4 | Validation & safety | 8-12 | High |
| 5 | Testing | 16-24 | Medium |
| **Total** | | **56-84** | **High** |

**Recommended approach**: Implement incrementally, test after each phase

---

**Specification Version**: 1.0
**Created**: 2026-03-31
**Status**: READY FOR IMPLEMENTATION
**Next Action**: Deploy agent for task #140 after tasks #133-139 complete
