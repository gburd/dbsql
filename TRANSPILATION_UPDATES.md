# Transpilation Tools - SQLite 3.52.0 Updates

This document describes required updates to the transpilation tools in `dist/transpile/` to support the SQLite 3.52.0 modernization.

## Status

**Current State:** Tools functional but lack SQLite 3.52.0 structure mappings
**Priority:** Medium (not blocking, but needed for future SQLite code imports)
**Estimated Effort:** 20-30 hours total

## Tools Overview

| Tool | Purpose | Status | Updates Needed |
|------|---------|--------|----------------|
| sqlite_to_dbsql.py | Maps SQLite→DBSQL conventions | ⚠️ Functional | Add 50+ field mappings |
| verify_transpilation.py | Validates naming/patterns | ⚠️ Functional | Add VDBE validation rules |
| name_mapping.json | Documentation of mappings | ⚠️ Functional | Document 30+ new fields |

## 1. sqlite_to_dbsql.py Updates

**File:** `dist/transpile/sqlite_to_dbsql.py`

### Add SQLite 3.52.0 Structure Field Mappings

#### VdbeOp Structure (lines ~150)

```python
# VdbeOp structure - SQLite 3.52.0 additions
STRUCTURE_FIELD_MAPPINGS = {
    # Existing mappings...

    # VdbeOp fields (SQLite 3.52.0)
    "VdbeOp.p5": "vdbe_op_t.p5",          # New flags field (u16)
    "VdbeOp.p4type": "vdbe_op_t.p4type",  # Type discriminator
    "VdbeOp.p4": "vdbe_op_t.p4",          # Union parameter
    "VdbeOp.comment": "vdbe_op_t.zComment", # Debug comment
}
```

#### Mem Structure (lines ~180)

```python
# Mem structure - SQLite 3.52.0 changes
STRUCTURE_FIELD_MAPPINGS.update({
    "Mem.u": "mem_t.u",                   # Union (i64/double/nZero/pDef)
    "Mem.u.i": "mem_t.u.i",               # Integer value
    "Mem.u.r": "mem_t.u.r",               # Real value
    "Mem.u.nZero": "mem_t.u.nZero",       # Zero-blob size
    "Mem.szMalloc": "mem_t.sz_malloc",    # Malloc size
    "Mem.uTemp": "mem_t.u_temp",          # Temp value
    "Mem.eSubtype": "mem_t.e_subtype",    # JSON subtype
    "Mem.enc": "mem_t.enc",               # Encoding (UTF8/UTF16)
    "Mem.xDel": "mem_t.xDel",             # Destructor callback
})
```

#### Parser Structure (lines ~220)

```python
# Parse structure - SQLite 3.52.0 additions
STRUCTURE_FIELD_MAPPINGS.update({
    "Parse.nRangeReg": "parser_t.n_range_reg",
    "Parse.iRangeReg": "parser_t.i_range_reg",
    "Parse.nLabelAlloc": "parser_t.n_label_alloc",
    "Parse.nQueryLoop": "parser_t.n_query_loop",
    "Parse.isMultiWrite": "parser_t.is_multi_write",
    "Parse.eTriggerOp": "parser_t.e_trigger_op",
    "Parse.eOrconf": "parser_t.e_orconf",
    "Parse.selId": "parser_t.sel_id",
})
```

#### Expr Structure (lines ~260)

```python
# Expr structure - SQLite 3.52.0 union changes
STRUCTURE_FIELD_MAPPINGS.update({
    "Expr.u.zToken": "expr_t.u.zToken.z",      # Token string
    "Expr.u.iValue": "expr_t.u.iValue",        # Integer value
    "Expr.x.pList": "expr_t.x.pList",          # Expression list
    "Expr.x.pSelect": "expr_t.x.pSelect",      # SELECT statement
    "Expr.y.pTab": "expr_t.y.pTab",            # Table reference
    "Expr.y.pWin": "expr_t.y.pWin",            # Window definition
    "Expr.w.iJoin": "expr_t.w.iJoin",          # Join table index
    "Expr.w.iOfst": "expr_t.w.iOfst",          /* Source offset */
})
```

### Add New Opcodes List (lines ~400)

```python
# SQLite 3.52.0 Opcodes (70+ new opcodes)
NEW_OPCODES_3_52_0 = [
    # Load/Store
    "OP_Move", "OP_Copy", "OP_SCopy",

    # Arithmetic
    "OP_Add", "OP_Subtract", "OP_Multiply", "OP_Divide",
    "OP_Remainder", "OP_Concat",

    # Comparison
    "OP_Eq", "OP_Ne", "OP_Lt", "OP_Le", "OP_Gt", "OP_Ge",

    # Control Flow
    "OP_If", "OP_IfNot", "OP_IfPos", "OP_IfNeg", "OP_IfZero",
    "OP_Gosub", "OP_Return", "OP_Yield",

    # Cursor Operations
    "OP_OpenRead", "OP_OpenWrite", "OP_Close",
    "OP_SeekGE", "OP_SeekGT", "OP_SeekLE", "OP_SeekLT",
    "OP_SeekRowid", "OP_NotExists", "OP_Found", "OP_NotFound",
    "OP_Rewind", "OP_Next", "OP_Prev", "OP_Last",

    # Data Operations
    "OP_Column", "OP_Rowid", "OP_NewRowid",
    "OP_MakeRecord", "OP_InsertInt", "OP_Delete",
    "OP_IdxInsert", "OP_IdxDelete", "OP_IdxRowid",

    # Results
    "OP_ResultRow", "OP_Halt", "OP_Integer", "OP_String",
    "OP_Null", "OP_Blob", "OP_Variable",

    # Aggregates
    "OP_AggStep", "OP_AggFinal", "OP_AggValue", "OP_AggInverse",

    # Sorting
    "OP_SorterOpen", "OP_SorterInsert", "OP_SorterSort",
    "OP_SorterData", "OP_SorterNext",

    # Functions
    "OP_Function", "OP_PureFunc",

    # Utilities
    "OP_Cast", "OP_Affinity", "OP_MustBeInt",
    "OP_BitAnd", "OP_BitOr", "OP_ShiftLeft", "OP_ShiftRight",
]
```

## 2. verify_transpilation.py Updates

**File:** `dist/transpile/verify_transpilation.py`

### Add VDBE Validation Functions (lines ~300)

```python
def validate_vdbe_opcodes(file_content, filename):
    """Validate VdbeOp structure initialization patterns."""
    errors = []

    # Check P5 flag initialization
    p5_pattern = r'\.p5\s*=\s*(?P<value>0x[0-9a-fA-F]+|\d+)'
    for match in re.finditer(p5_pattern, file_content):
        value = match.group('value')
        # P5 should be u16, check range
        int_val = int(value, 0)
        if int_val > 0xFFFF:
            errors.append(f"P5 value out of range (u16): {value}")

    # Validate register number ranges (0-255 typical)
    reg_pattern = r'(?:p1|p2|p3)\s*=\s*(?P<reg>\d+)'
    for match in re.finditer(reg_pattern, file_content):
        reg = int(match.group('reg'))
        if reg > 255:
            errors.append(f"Register number unusually high: {reg}")

    # Verify P4 union type consistency
    p4_patterns = [
        (r'\.p4type\s*=\s*P4_INT32', r'\.p4\.i\s*='),
        (r'\.p4type\s*=\s*P4_STRING', r'\.p4\.z\s*='),
        (r'\.p4type\s*=\s*P4_KEYINFO', r'\.p4\.pKeyInfo\s*='),
    ]
    # TODO: Implement cross-reference checking

    return errors
```

```python
def validate_mem_structure(file_content, filename):
    """Validate Mem (register) initialization patterns."""
    errors = []

    # Check flags set correctly
    flag_pattern = r'(?P<var>\w+)->flags\s*=\s*(?P<flags>MEM_\w+(?:\s*\|\s*MEM_\w+)*)'
    for match in re.finditer(flag_pattern, file_content):
        flags_str = match.group('flags')
        # Validate MEM_* flag combinations
        if 'MEM_Int' in flags_str and 'MEM_Real' in flags_str:
            errors.append(f"Invalid MEM flag combination: {flags_str}")

    # Ensure proper cleanup (xDel callback)
    malloc_pattern = r'(?P<var>\w+)->z\s*=\s*(?:malloc|__dbsql_malloc)'
    xdel_pattern = r'(?P<var>\w+)->xDel\s*='
    # TODO: Match malloc with xDel assignment

    return errors
```

```python
def validate_register_addressing(file_content, filename):
    """Validate register addressing is 0-indexed."""
    errors = []

    # Check for 1-indexed patterns (common mistake)
    suspicious_patterns = [
        r'(?:reg|iReg)\s*=\s*1;',  # Starting at 1
        r'(?:reg|iReg)\+\+;\s*//\s*(?:first|start)',  # Incrementing from assumed 0
    ]

    for pattern in suspicious_patterns:
        if re.search(pattern, file_content):
            errors.append(f"Suspicious 1-indexed register pattern: {pattern}")

    return errors
```

### Add Validation Entry Point (lines ~400)

```python
def validate_vdbe_code(file_path):
    """Main validation entry for VDBE-related files."""
    with open(file_path) as f:
        content = f.read()

    filename = os.path.basename(file_path)
    all_errors = []

    if 'vdbe' in filename.lower():
        all_errors.extend(validate_vdbe_opcodes(content, filename))
        all_errors.extend(validate_mem_structure(content, filename))
        all_errors.extend(validate_register_addressing(content, filename))

    if 'cg_' in filename:  # Code generators
        all_errors.extend(validate_register_addressing(content, filename))

    return all_errors
```

## 3. name_mapping.json Updates

**File:** `dist/transpile/name_mapping.json`

### Add SQLite 3.52.0 Structure Documentation (line ~50)

```json
{
  "vdbe_op_fields_3_52_0": {
    "p1": "Primary parameter (register, cursor, label)",
    "p2": "Secondary parameter (register, cursor, jump target)",
    "p3": "Tertiary parameter (register)",
    "p4": "Union parameter (string, int, pointer, keyinfo)",
    "p4type": "P4 union discriminator (P4_INT32, P4_STRING, P4_KEYINFO, etc.)",
    "p5": "Flags (u16 bitfield for opcode-specific options)",
    "comment": "Debug comment string (only if SQLITE_ENABLE_EXPLAIN_COMMENTS)"
  },

  "mem_fields_3_52_0": {
    "flags": "Type flags (MEM_Int, MEM_Real, MEM_Str, MEM_Blob, etc.)",
    "enc": "String encoding (SQLITE_UTF8, SQLITE_UTF16LE, SQLITE_UTF16BE)",
    "eSubtype": "Subtype for JSON/custom types (0=none)",
    "n": "String length (bytes) or blob size",
    "z": "String/blob pointer (may be zMalloc or external)",
    "zMalloc": "Owned malloc buffer (if MEM_Dyn flag set)",
    "szMalloc": "Size of zMalloc buffer",
    "u": "Union of i64/double/nZero/pDef",
    "u.i": "Integer value (if MEM_Int)",
    "u.r": "Real (double) value (if MEM_Real)",
    "u.nZero": "Zero-blob size (if MEM_Zero)",
    "u.pDef": "Function definition pointer (if MEM_Agg)",
    "uTemp": "Temporary value for operations",
    "xDel": "Destructor callback for external memory"
  },

  "parser_fields_3_52_0": {
    "nRangeReg": "Size of temporary register block",
    "iRangeReg": "First register in temp register block",
    "nLabelAlloc": "Number of slots allocated in aLabel array",
    "aLabel": "Space to hold labels (array of int)",
    "nLabel": "*Negative* of number of labels used",
    "nQueryLoop": "Estimated iterations of query (10*log2(N))",
    "isMultiWrite": "True if may modify/insert multiple rows",
    "eTriggerOp": "TK_UPDATE, TK_INSERT, or TK_DELETE",
    "eOrconf": "Default ON CONFLICT policy for triggers",
    "selId": "Unique identifier for SELECT statements"
  },

  "expr_fields_3_52_0": {
    "op": "Operation performed by this node (TK_* token)",
    "op2": "TK_REGISTER: original op; TK_COLUMN: p5 for OP_Column",
    "flags": "Various flags (EP_*)",
    "u": "Union of zToken (struct {z, n}), zTokenStr, or iValue",
    "u.zToken": "Token with explicit length (anonymous struct)",
    "u.zToken.z": "Token string pointer",
    "u.zToken.n": "Token length",
    "u.iValue": "Non-negative integer value (if EP_IntValue)",
    "x": "Union of pList or pSelect",
    "x.pList": "Expression list (for IN, CASE, FUNCTION, etc.)",
    "x.pSelect": "SELECT statement (if EP_xIsSelect)",
    "y": "Union of pTab, pWin, nReg, or sub",
    "y.pTab": "Table containing column (TK_COLUMN)",
    "y.pWin": "Window/Filter definition (EP_WinFunc)",
    "w": "Union of iJoin or iOfst",
    "w.iJoin": "Right table in join (if EP_OuterON/EP_InnerON)",
    "w.iOfst": "Token offset from statement start (else)",
    "pAggInfo": "Aggregate info (TK_AGG_COLUMN/TK_AGG_FUNCTION)",
    "iTable": "TK_COLUMN: cursor; TK_REGISTER: register; TK_IN: ephemeral table",
    "iColumn": "TK_COLUMN: column index (-1=rowid); TK_VARIABLE: variable number",
    "iAgg": "Entry in pAggInfo->aCol[] or ->aFunc[]"
  }
}
```

## Implementation Checklist

### Phase 1: Structure Mappings (8-12 hours)

- [ ] Add VdbeOp field mappings to sqlite_to_dbsql.py
- [ ] Add Mem field mappings
- [ ] Add Parser field mappings
- [ ] Add Expr field mappings
- [ ] Add 70+ new opcode names
- [ ] Test mappings with sample SQLite 3.52.0 code

### Phase 2: Validation Rules (8-12 hours)

- [ ] Implement validate_vdbe_opcodes()
- [ ] Implement validate_mem_structure()
- [ ] Implement validate_register_addressing()
- [ ] Add validation entry point
- [ ] Write unit tests for validators
- [ ] Test on existing DBSQL code

### Phase 3: Documentation (4-6 hours)

- [ ] Add VdbeOp field documentation to name_mapping.json
- [ ] Add Mem field documentation
- [ ] Add Parser field documentation
- [ ] Add Expr field documentation
- [ ] Review and verify all mappings

## Testing

After updates, test the tools:

```bash
# Test sqlite_to_dbsql.py
cd dist/transpile
./sqlite_to_dbsql.py --test

# Test verify_transpilation.py on DBSQL codebase
./verify_transpilation.py ../../src/vdbe.c
./verify_transpilation.py ../../src/cg_*.c

# Verify name_mapping.json is valid JSON
python3 -m json.tool name_mapping.json > /dev/null && echo "Valid JSON"
```

## Future Work

Beyond SQLite 3.52.0 mappings:

1. **Automated mapping generation** - Extract from SQLite headers
2. **Diff-based transpilation** - Track SQLite→DBSQL diffs per file
3. **Bidirectional mappings** - Support DBSQL→SQLite for upstreaming
4. **CI integration** - Run verification on every commit

## References

- SQLite 3.52.0 source: https://sqlite.org/releaselog/3_52_0.html
- DBSQL modernization plan: POST_MODERNIZATION_PLAN_STATUS.md
- Structure definitions: src/inc/dbsql_int.in
- VDBE architecture: docs/VDBE_ARCHITECTURE.md
