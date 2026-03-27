# Memory Allocation Bug Analysis

## The Problem

DBSQL uses Berkeley DB's custom memory allocators (`__os_malloc`, `__os_free`) which require a `DB_ENV*` context. However, many places in the code allocate memory with `__dbsql_strdup(NULL, ...)` which uses standard `malloc()`, but this memory is later freed with `__os_free(dbenv, ...)`, causing "free(): invalid pointer" crashes.

## Root Cause

The function signature is:
```c
int __dbsql_strdup(DBSQL *dbp, const char *str, void **ptr);
```

When `dbp` is NULL, it uses standard `malloc()`. When `dbp` is non-NULL, it uses `__os_malloc(dbp->dbenv, ...)`.

**Problem**: Code that passes NULL for convenience gets standard malloc'd memory, but Berkeley DB later tries to free it with `__os_free(dbenv, ...)`.

## All Instances Found

### Fixed (1 instance)
1. **src/api.c:350** - `__dbsql_strdup(NULL, name, &p->zName);`
   - **Fix**: Changed to `__dbsql_strdup(dbp, name, &p->zName);`
   - **Status**: FIXED in commit 8a8719d

### Remaining Issues (15+ instances)

#### src/cg_select.c (3 instances)
- Line 2915: `__dbsql_strdup(NULL, tab_name, &table->zName)`
- Line 2927: `__dbsql_strdup(NULL, elist->a[i].zName, &column[i].zName)`
- Line 2950: `__dbsql_strdup(NULL, buf, &table->aCol[i].zName)`

**Context**: These are in `__generate_column_names()` function which has `parser_t *parser` available.
**Fix**: Use `parser->db` instead of NULL

#### src/cg_expr.c (7 instances)
- Line 181: `__dbsql_strdup(NULL, p->u.zToken.z, &new->u.zToken.z)`
- Line 251: `__dbsql_strdup(NULL, p->a[i].zName, &new->a[i].zName)`
- Line 281: `__dbsql_strdup(NULL, old_item->zDatabase, &new_item->zDatabase)`
- Line 282: `__dbsql_strdup(NULL, old_item->zName, &new_item->zName)`
- Line 283: `__dbsql_strdup(NULL, old_item->zAlias, &new_item->zAlias)`
- Line 315: `__dbsql_strdup(NULL, old_item->zName, &new_item->zName)`
- Line 783: `__dbsql_strdup(NULL, col_name, &z)`

**Context**: Expression duplication functions, most have `DBSQL *dbp` parameter available.
**Fix**: Use `dbp` instead of NULL

#### src/vdbe_method.c (2 instances)
- Line 559: `__dbsql_strdup(NULL, op->p4.z, (char**)&op->p4.z)`
- Line 588: `__dbsql_strdup(NULL, op->p4.z, (char**)&op->p4.z)`

**Context**: In `__vdbe_make_ready()` which has `vdbe_t *vm` with `vm->db` available.
**Fix**: Use `vm->db` instead of NULL

#### src/dbsql/dbsql.c (1 instance)
- Line 1420: `__dbsql_strdup(NULL, argv[i], &data.crypt_key)`

**Context**: In `main()` before DB environment is created. This is BEFORE `dbsql_create_env()`.
**Fix**: This is tricky - can't use dbenv since it doesn't exist yet. Options:
  1. Keep as NULL (standard malloc) but ensure it's freed with standard free() not __os_free()
  2. Allocate after dbenv is created
  3. Use strdup() directly and document it's not OS-allocated

## Systematic Fix Strategy

### Phase 1: Fix code generator files (safe, have DBSQL* available)
1. Fix cg_select.c - use `parser->db`
2. Fix cg_expr.c - use `dbp` parameter
3. Fix vdbe_method.c - use `vm->db`

### Phase 2: Fix dbsql.c main() (requires special handling)
The crypt_key allocation happens before dbenv exists. Need to either:
- Move the allocation after `dbsql_create_env()`
- Or ensure it's freed with regular free(), not __os_free()

## Testing After Fixes

```bash
# Test 1: Simple CREATE TABLE
mkdir -p ~/test_sql && cd ~/test_sql
echo "CREATE TABLE test (id INTEGER);" | ./dbsql test.db

# Test 2: CRUD operations
echo "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30);
INSERT INTO users VALUES (2, 'Bob', 25);
SELECT * FROM users;
SELECT * FROM users ORDER BY age;" | ./dbsql test.db

# Test 3: Expression handling (tests cg_expr.c fixes)
echo "SELECT 1+2, 'hello' || ' world';" | ./dbsql :memory:
```

## Priority

**CRITICAL** - These bugs prevent ANY database operation from working. Must fix all instances before SQL can be tested.

## Commits Needed

1. ✅ Commit 8a8719d: Fixed api.c:350
2. TODO: Fix cg_select.c (3 instances)
3. TODO: Fix cg_expr.c (7 instances)
4. TODO: Fix vdbe_method.c (2 instances)
5. TODO: Fix dbsql.c:1420 (special case)

## Verification

After all fixes, this command should work without "free(): invalid pointer":
```bash
echo "CREATE TABLE test (id INTEGER);" | ./dbsql test.db
```
