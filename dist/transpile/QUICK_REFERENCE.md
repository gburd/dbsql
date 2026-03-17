# Transpilation Tools Quick Reference

## Tool Overview

| Tool | Purpose | Usage |
|------|---------|-------|
| sqlite_to_dbsql.py | Convert SQLite → DBSQL | `./sqlite_to_dbsql.py -i in.c -o out.c` |
| verify_transpilation.py | Validate DBSQL code | `./verify_transpilation.py file.c` |
| name_mapping.json | Documentation | Reference for mappings |

## Common Commands

```bash
# Transpile a file
./sqlite_to_dbsql.py -i sqlite_file.c -o dbsql_file.c

# Transpile without line wrapping
./sqlite_to_dbsql.py -i in.c -o out.c --no-line-wrap

# Dry run (print to stdout)
./sqlite_to_dbsql.py -i in.c -o /dev/null --dry-run

# Run self-tests
./sqlite_to_dbsql.py --test

# Verify a file
./verify_transpilation.py src/vdbe.c

# Verify directory
./verify_transpilation.py src/

# Verify with options
./verify_transpilation.py file.c --no-line-length --no-markers

# Summary only
./verify_transpilation.py file.c --summary-only

# Check JSON
python3 -m json.tool name_mapping.json > /dev/null
```

## Conversion Cheat Sheet

### Types
```
u8, u16, u32    → u_int8_t, u_int16_t, u_int32_t
i64, u64        → int64_t, uint64_t
Parse           → parser_t
Expr            → expr_t
Vdbe            → vdbe_t
Mem             → mem_t
VdbeOp          → vdbe_op_t
```

### Functions
```
sqliteVdbeExec      → __vdbe_exec
sqlite3GetVdbe      → __get_vdbe
sqliteExprListAppend → __expr_list_append
```

### Fields
```
szMalloc        → sz_malloc
nRangeReg       → n_range_reg
isMultiWrite    → is_multi_write
eSubtype        → e_subtype
comment         → zComment (special)
```

### Constants
```
SQLITE_OK       → DBSQL_OK
SQLITE_ERROR    → DBSQL_ERROR
```

## Verification Checks

| Check | Description | Flag to Disable |
|-------|-------------|-----------------|
| Function naming | __ prefix required | (none) |
| Type naming | _t suffix required | (none) |
| BDB types | Must be preserved | (none) |
| Header markers | PUBLIC/STATIC/EXTERN | --no-markers |
| Line length | Max 79 chars | --no-line-length |
| ANSI C | No K&R style | --no-knr |
| VDBE validation | P5/MEM/register checks | --no-vdbe |

## Field Name Patterns

```
CamelCase       → camel_case
nPrefix         → n_prefix     (count/number)
iPrefix         → i_prefix     (index)
pPrefix         → p_prefix     (pointer)
aPrefix         → a_prefix     (array)
zPrefix         → z_prefix     (zero-terminated string)
ePrefix         → e_prefix     (enum)
```

## VDBE Validation Rules

### P5 Flags
- Must be 0x0000-0xFFFF (u16 range)
- Opcode-specific bitfield

### MEM Flags
- Cannot mix MEM_Int + MEM_Real
- Cannot mix MEM_Str + MEM_Blob

### Registers
- 0-indexed (not 1-indexed)
- Typically 0-255 range
- Warning if > 255

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success / All tests pass |
| 1 | Failure / Violations found |

## File Structure

```
dist/transpile/
├── sqlite_to_dbsql.py       # Main transpilation tool
├── verify_transpilation.py  # Validation tool
├── name_mapping.json        # Documentation
├── EXAMPLES.md              # Usage examples
└── QUICK_REFERENCE.md       # This file
```

## Workflow

```
┌─────────────────┐
│ SQLite 3.52.0   │
│   source.c      │
└────────┬────────┘
         │
         │ sqlite_to_dbsql.py
         │
         ▼
┌─────────────────┐
│ DBSQL source.c  │
└────────┬────────┘
         │
         │ verify_transpilation.py
         │
         ▼
┌─────────────────┐
│ ✓ Verified      │
│   DBSQL code    │
└─────────────────┘
```

## SQLite 3.52.0 Structures

### VdbeOp (vdbe_op_t)
```c
struct VdbeOp {
    u8 opcode;     // Opcode number
    u8 p4type;     // P4 type discriminator
    u16 p5;        // Flags (NEW in 3.52.0)
    int p1;        // First parameter
    int p2;        // Second parameter
    int p3;        // Third parameter
    union {        // P4 union
        int i;
        char *z;
        KeyInfo *pKeyInfo;
        // ...
    } p4;
    char *comment; // Debug comment → zComment
};
```

### Mem (mem_t)
```c
struct Mem {
    u16 flags;        // Type flags
    u8 enc;           // Encoding
    u8 eSubtype;      // Subtype (NEW in 3.52.0)
    int n;            // String/blob length
    char *z;          // Data pointer
    char *zMalloc;    // Malloc buffer
    int szMalloc;     // Malloc size → sz_malloc
    u32 uTemp;        // Temp value → u_temp
    union {           // Value union
        i64 i;
        double r;
        int nZero;
        FuncDef *pDef;
    } u;
    void (*xDel)(void*); // Destructor
};
```

### Parse (parser_t)
```c
struct Parse {
    // ... existing fields ...
    int nRangeReg;    // Register block size → n_range_reg
    int iRangeReg;    // First register → i_range_reg
    int nLabelAlloc;  // Label array size → n_label_alloc
    int nQueryLoop;   // Query iterations → n_query_loop
    u8 isMultiWrite;  // Multi-row flag → is_multi_write
    u8 eTriggerOp;    // Trigger op → e_trigger_op
    u8 eOrconf;       // ON CONFLICT → e_orconf
    u32 selId;        // SELECT id → sel_id
};
```

### Expr (expr_t)
```c
struct Expr {
    u8 op;            // Operation
    u8 op2;           // Secondary op
    u32 flags;        // Flags
    union {           // Token/value union
        struct {      // Anonymous struct
            char *z;  // Token string
            int n;    // Token length
        } zToken;     // pExpr->u.zToken.z in DBSQL
        int iValue;
    } u;
    union {           // List/select union
        ExprList *pList;   // → p_list
        Select *pSelect;   // → p_select
    } x;
    union {           // Table/window union
        Table *pTab;       // → p_tab
        Window *pWin;      // → p_win
    } y;
    union {           // Join/offset union
        int iJoin;         // → i_join
        int iOfst;         // → i_ofst
    } w;
};
```

## Tips

1. **Always run --test first** to verify tool is working
2. **Use --dry-run** to preview changes before writing
3. **Verify immediately** after transpilation
4. **Check line length** issues can be deferred with --no-line-length
5. **Review field renames** especially for union members
6. **Test incrementally** on small files first

## Support

- Documentation: TRANSPILATION_UPDATES.md
- Examples: EXAMPLES.md
- Completion report: TRANSPILATION_TOOLS_UPDATE_COMPLETE.md
- Structure definitions: ../src/inc/dbsql_int.in
- VDBE docs: ../docs/VDBE_ARCHITECTURE.md
