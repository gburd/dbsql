# Critical Fix Priority List - Task #26

## CRITICAL BLOCKER #1: Header File Structure (dbsql_int.in)

### Issue: Duplicate Struct Definitions
**Files**: `/home/gburd/ws/dbsql/src/inc/dbsql_int.in` and generated `/home/gburd/ws/dbsql/build_unix/dbsql_int.h`

**Problem**: Five structs defined twice in same file:

```
Line 1599: struct autoinc_info { ... }     [KEEP THIS]
Line 2173: struct autoinc_info { ... }     [DELETE THIS]

Line 1616: struct trigger_prg { ... }      [KEEP THIS]
Line 2532: struct trigger_prg { ... }      [DELETE THIS]

Line 1634: struct indexed_expr { ... }     [KEEP THIS]
Line 2544: struct indexed_expr { ... }     [DELETE THIS]

Line 1652: struct parse_cleanup { ... }    [KEEP THIS]
Line 2384: struct parse_cleanup { ... }    [DELETE THIS]

Line 1662: struct returning { ... }        [KEEP THIS]
Line 2294: struct returning { ... }        [DELETE THIS]
```

**Action**: Remove lines 2173-2178, 2294-2303, 2384-2388, 2532-2540, 2544-2564 from dbsql_int.in

### Issue: Macro Definition Order
**Problem**: Macro used before defined
```
Line 2562: func_def_t *a[DBSQL_FUNC_HASH_SZ];  /* Uses macro */
Line 2566: #define DBSQL_FUNC_HASH_SZ 61        /* Defines macro */
```

**Action**: Move lines 2565-2567 to before line 2562

### Issue: Incomplete Type in Embedded Struct
**Problem**: Cannot embed incomplete types
```
Line 1665: trigger_t retTrig;        /* trigger_t not yet defined */
Line 1666: trigger_step_t retTStep;  /* trigger_step_t not yet defined */
```

**Action**: Change to pointers:
```c
struct returning {
    parser_t *pParse;
    expr_list_t *pReturnEL;
    trigger_t *pRetTrig;       /* Changed to pointer */
    trigger_step_t *pRetTStep; /* Changed to pointer */
    int iRetCur;
    int nRetCol;
    int iRetReg;
    char zName[40];
};
```

### Issue: Include Order Problem
**Problem**: `mem_t` not defined when `dbsql_ext.h` included
```
Line 2677: #include "inc/dbsql_ext.h"  /* Uses mem_t in function prototypes */
```

But `mem_t` defined in `vdbe_int.h` which is never included in `dbsql_int.h`

**Action**: Add near line 50 (after other includes):
```c
#include "inc/vdbe_int.h"
```

## CRITICAL FIX SCRIPT

```bash
#!/bin/bash
# fix_headers.sh - Fix critical header file issues

cd /home/gburd/ws/dbsql

# 1. Remove duplicate struct definitions
sed -i '2173,2178d' src/inc/dbsql_int.in  # Remove duplicate autoinc_info
sed -i '2294,2303d' src/inc/dbsql_int.in  # Remove duplicate returning
sed -i '2384,2388d' src/inc/dbsql_int.in  # Remove duplicate parse_cleanup
sed -i '2532,2540d' src/inc/dbsql_int.in  # Remove duplicate trigger_prg
sed -i '2544,2564d' src/inc/dbsql_int.in  # Remove duplicate indexed_expr

# 2. Fix returning struct - change embedded to pointers
sed -i 's/trigger_t retTrig;/trigger_t *pRetTrig;/' src/inc/dbsql_int.in
sed -i 's/trigger_step_t retTStep;/trigger_step_t *pRetTStep;/' src/inc/dbsql_int.in

# 3. Add vdbe_int.h include (after line with globals.h)
sed -i '/inc\/globals.h/a #include "inc/vdbe_int.h"' src/inc/dbsql_int.in

# 4. Move DBSQL_FUNC_HASH_SZ definition before usage
# This requires more complex editing - manual fix recommended

# Regenerate headers
cd build_unix
./config.status

echo "Header fixes applied. Review changes before committing."
```

**MANUAL FIX REQUIRED**: Steps 1-3 above, then manually move DBSQL_FUNC_HASH_SZ definition.

## HIGH PRIORITY FIX #2: Expression Structure Access Patterns

### Search/Replace Patterns (Global)

**Pattern 1**: expr->token → expr->u.zToken (79 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->token/\1->u.zToken/g' {} \;
```

**Pattern 2**: expr->pList → expr->x.pList (78 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->pList/\1->x.pList/g' {} \;
```

**Pattern 3**: expr->pSelect → expr->x.pSelect (23 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->pSelect/\1->x.pSelect/g' {} \;
```

**Pattern 4**: Remove expr->span references (61 occurrences)
This requires case-by-case analysis - span field removed in SQLite 3.52.0

**Files Affected**:
- All cg_*.c files
- sql_parser.y
- sql_tokenize.c

## HIGH PRIORITY FIX #3: Memory Value Structure Access

**Pattern 1**: mem->i → mem->u.i (57 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->i\([^a-zA-Z0-9_]\)/\1->u.i\2/g' {} \;
```

**Pattern 2**: mem->r → mem->u.r (23 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->r\([^a-zA-Z0-9_]\)/\1->u.r\2/g' {} \;
```

**Pattern 3**: Remove mem->zShort references (34 occurrences)
This field removed - requires case-by-case analysis

**Files Affected**:
- vdbe.c
- vdbe_method.c
- All cg_*.c files using VDBE operations

## HIGH PRIORITY FIX #4: Cursor Type Names

**Pattern**: VdbeCursor → cursor_t (121 occurrences total)
```bash
find src -name "*.c" -type f -exec sed -i 's/\bVdbeCursor\b/cursor_t/g' {} \;
find src -name "*.h" -type f -exec sed -i 's/\bVdbeCursor\b/cursor_t/g' {} \;
```

## HIGH PRIORITY FIX #5: VDBE Structure Access

**Pattern 1**: vdbe->aCsr → vdbe->apCsr (32 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->aCsr/\1->apCsr/g' {} \;
```

**Pattern 2**: Remove vdbe->aStack references (49 occurrences)
New stack management - requires reimplementation

**Pattern 3**: Remove vdbe->agg references (31 occurrences)
New aggregate system - requires reimplementation

**Files Affected**:
- vdbe.c (main implementation)
- vdbe_method.c (method implementation)

## MEDIUM PRIORITY FIX #6: Schema Structure Access

**Pattern 1**: column->zName → column->hName (35 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->zName/\1->hName/g' {} \;
```

**Pattern 2**: Remove table->iDb references (67 occurrences)
Database index tracking moved - requires context-specific fixes

**Pattern 3**: Remove index->iDb references (26 occurrences)
Database index tracking moved - requires context-specific fixes

**Pattern 4**: Remove table->pSelect references (25 occurrences)
View handling changed - requires context-specific fixes

**Files Affected**:
- All cg_*.c files
- api.c, api_table.c

## MEDIUM PRIORITY FIX #7: Function Signature Updates

**Issue**: __vdbe_change_p3() signature changed (71 occurrences)

Old signature:
```c
void __vdbe_change_p3(vdbe_t *p, int addr, const char *zP3, int n);
```

New signature (check vdbe.h):
```c
void __vdbe_change_p3(vdbe_t *p, int addr, int p3);
```

This requires case-by-case analysis of each call site.

**Files Affected**:
- All cg_*.c files
- vdbe_method.c

## MEDIUM PRIORITY FIX #8: Parser Integration

**Pattern 1**: Remove parser->trigStack references (27 occurrences)
Trigger handling changed - requires reimplementation

**Pattern 2**: select->nLimit → select->iLimit (26 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->nLimit/\1->iLimit/g' {} \;
```

**Pattern 3**: expr_item->zName → expr_item->zEName (22 occurrences)
```bash
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->zName/\1->zEName/g' {} \;
```

**Files Affected**:
- cg_select.c
- cg_trigger.c
- sql_parser.y

## SYSTEMATIC FIX ORDER

1. **IMMEDIATE**: Fix header files manually (1 hour)
   - Edit dbsql_int.in directly
   - Regenerate with config.status
   - Verify no redefinition errors

2. **BATCH 1**: Run automated search/replace (30 minutes)
   - expr->token → expr->u.zToken
   - expr->pList → expr->x.pList
   - expr->pSelect → expr->x.pSelect
   - mem->i → mem->u.i
   - mem->r → mem->u.r
   - VdbeCursor → cursor_t
   - vdbe->aCsr → vdbe->apCsr
   - column->zName → column->hName
   - select->nLimit → select->iLimit

3. **BATCH 2**: Manual fixes for removed fields (4-6 hours)
   - expr->span removal (61 locations)
   - mem->zShort removal (34 locations)
   - vdbe->aStack removal (49 locations)
   - vdbe->agg removal (31 locations)
   - table->iDb removal (67 locations)
   - index->iDb removal (26 locations)
   - table->pSelect removal (25 locations)
   - parser->trigStack removal (27 locations)

4. **BATCH 3**: Function signature updates (2-3 hours)
   - __vdbe_change_p3() calls (71 locations)
   - Other VDBE API updates

5. **TEST**: Incremental build validation
   - After each batch, attempt build
   - Fix any new errors revealed

## VERIFICATION SCRIPT

```bash
#!/bin/bash
# verify_fixes.sh - Check if critical fixes applied

echo "Checking for critical errors..."

# Check for duplicate definitions
echo -n "Duplicate struct definitions: "
grep -c "^struct autoinc_info {" build_unix/dbsql_int.h

# Check for mem_t definition before use
echo -n "mem_t defined before dbsql_ext.h: "
grep -n "typedef.*mem_t\|#include.*dbsql_ext.h" build_unix/dbsql_int.h | head -2

# Check for old field access patterns
echo -n "Old expr->token patterns: "
grep -r "->token" src/*.c | wc -l

echo -n "Old expr->pList patterns (should be x.pList): "
grep -r "->pList" src/*.c | grep -v "x.pList" | wc -l

echo "Done."
```
