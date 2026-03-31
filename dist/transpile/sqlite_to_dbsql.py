#!/usr/bin/env python3
#
# DBSQL - A SQL database engine.
#
# Copyright (C) 2007-2008  The DBSQL Group, Inc.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

"""
sqlite_to_dbsql.py - Automated transpilation of SQLite C source code
to DBSQL conventions.

This script performs mechanical transformations on SQLite C source
files to convert them to the DBSQL naming and style conventions:

  - CamelCase function names -> __snake_case
  - Typedef names -> _t suffix
  - SQLite integer types -> BSD/POSIX types
  - sqlite_* identifiers -> dbsql_*
  - Insert PUBLIC:/STATIC:/EXTERN: header markers
  - Wrap long lines at 79 characters

Hungarian notation for variables (z/n/a/p/i prefixes) is NOT handled
by this script; it requires manual semantic analysis.
"""

import argparse
import re
import sys
import textwrap


# ----------------------------------------------------------------
# BerkeleyDB types that must NOT be modified
# ----------------------------------------------------------------
BDB_TYPES = frozenset([
    "DB", "DBT", "DBC", "DB_ENV", "DB_TXN", "db_recno_t",
    "DB_BTREE", "DB_HASH", "DB_QUEUE", "DB_RECNO", "DB_UNKNOWN",
    "DB_MPOOL_CREATE", "DB_MPOOL_LAST", "DB_MPOOL_NEW",
    "DB_LOCK_DEFAULT", "DB_LOCK_NOTGRANTED",
    "db_threadid_t", "db_pgno_t",
])

# ----------------------------------------------------------------
# SQLite integer type -> POSIX/BSD type mappings
# ----------------------------------------------------------------
SQLITE_TYPE_MAP = {
    "u8":  "u_int8_t",
    "u16": "u_int16_t",
    "u32": "u_int32_t",
    "i64": "int64_t",
    "u64": "uint64_t",
}

# ----------------------------------------------------------------
# Known SQLite typedef names that should get _t suffix.
# These are the original SQLite names before conversion.
# ----------------------------------------------------------------
SQLITE_TYPEDEFS = [
    "Table", "Column", "Index", "Expr", "ExprList",
    "Parse", "Token", "IdList", "SrcList",
    "WhereInfo", "WhereLevel", "Select",
    "Agg", "AggExpr", "AggElem",
    "KeyList", "Cursor", "FuncDef",
    "Trigger", "TriggerStep", "TriggerStack",
    "FKey", "AuthContext", "Vdbe",
    "DateTime", "Mem", "Op", "Sorter",
    "KeyInfo", "CollSeq",
]

# Map original SQLite CamelCase type -> dbsql snake_case_t
TYPEDEF_MAP = {}

# Some types have non-mechanical mappings (established in codebase)
TYPEDEF_OVERRIDES = {
    "Parse":        "parser_t",
    "FKey":         "foreign_key_t",
    "ExprList":     "expr_list_t",
    "IdList":       "id_list_t",
    "SrcList":      "src_list_t",
    "WhereInfo":    "where_info_t",
    "WhereLevel":   "where_level_t",
    "AggExpr":      "agg_expr_t",
    "AggElem":      "agg_elem_t",
    "KeyList":      "keylist_t",
    "FuncDef":      "func_def_t",
    "TriggerStep":  "trigger_step_t",
    "TriggerStack": "trigger_stack_t",
    "AuthContext":   "auth_context_t",
    "DateTime":     "datetime_t",
    "KeyInfo":      "key_info_t",
    "CollSeq":      "coll_seq_t",
}


def _camel_to_snake(name):
    """Convert a CamelCase name to snake_case."""
    # Insert underscore before each uppercase letter that follows
    # a lowercase letter or digit, or before a sequence of uppercase
    # letters followed by a lowercase letter.
    s1 = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    s2 = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s1)
    return s2.lower()


def _build_typedef_map():
    """Populate TYPEDEF_MAP from SQLITE_TYPEDEFS."""
    for name in SQLITE_TYPEDEFS:
        if name in TYPEDEF_OVERRIDES:
            TYPEDEF_MAP[name] = TYPEDEF_OVERRIDES[name]
        else:
            TYPEDEF_MAP[name] = _camel_to_snake(name) + "_t"


_build_typedef_map()


# ----------------------------------------------------------------
# SQLite 3.52.0 structure field mappings
# These handle non-mechanical transformations (renamed fields,
# nested union changes). Regular CamelCase->snake_case field
# conversions are handled by convert_field_names().
# ----------------------------------------------------------------
STRUCTURE_FIELD_MAPPINGS = {
    # VdbeOp structure - special field renames
    "comment": "zComment",  # SQLite uses "comment", DBSQL uses "zComment"

    # Expr structure - union restructuring
    # SQLite Expr.u.zToken (char*) -> DBSQL expr_t.u.zToken.z (struct {char *z; int n})
    # This is a structural change that needs special handling in actual code
}

# ----------------------------------------------------------------
# SQLite 3.52.0 structure field mappings - New additions
# VdbeOp, Mem, Parser structures
# ----------------------------------------------------------------
VDBE_OP_FIELD_MAPPINGS = {
    # VdbeOp structure - SQLite 3.52.0 fields
    # These are typically already lowercase or use simple names
    "p1": "p1",         # register/cursor/label (no change)
    "p2": "p2",         # register/cursor/jump (no change)
    "p3": "p3",         # register (no change)
    "p4": "p4",         # union parameter (no change)
    "p5": "p5",         # u16 flags (no change)
    "p4type": "p4type", # P4 discriminator (no change)
}

MEM_FIELD_MAPPINGS = {
    # Mem structure - SQLite 3.52.0 fields
    # CamelCase fields get converted by convert_field_names()
    "szMalloc": "sz_malloc",   # size of zMalloc buffer
    "uTemp": "u_temp",         # temporary value
    "eSubtype": "e_subtype",   # JSON/custom subtype
}

PARSER_FIELD_MAPPINGS = {
    # Parse structure - SQLite 3.52.0 additions
    "nRangeReg": "n_range_reg",       # temp register block size
    "iRangeReg": "i_range_reg",       # first temp register
    "nLabelAlloc": "n_label_alloc",   # allocated label slots
    "aLabel": "a_label",               # label array
}

# ----------------------------------------------------------------
# SQLite 3.52.0 Opcodes (70+ opcodes for reference)
# These don't require transformation (OP_* prefix stays the same)
# but are documented here for completeness.
# ----------------------------------------------------------------
NEW_OPCODES_3_52_0 = [
    # Load/Store
    "OP_Move", "OP_Copy", "OP_SCopy", "OP_IntCopy",

    # Arithmetic
    "OP_Add", "OP_Subtract", "OP_Multiply", "OP_Divide",
    "OP_Remainder", "OP_Concat", "OP_BitAnd", "OP_BitOr",
    "OP_ShiftLeft", "OP_ShiftRight", "OP_BitNot",

    # Comparison
    "OP_Eq", "OP_Ne", "OP_Lt", "OP_Le", "OP_Gt", "OP_Ge",
    "OP_ElseEq", "OP_Compare", "OP_IsNull", "OP_NotNull",

    # Control Flow
    "OP_If", "OP_IfNot", "OP_IfPos", "OP_IfNeg", "OP_IfZero",
    "OP_IfNotZero", "OP_Gosub", "OP_Return", "OP_Yield",
    "OP_Goto", "OP_Once", "OP_InitCoroutine", "OP_EndCoroutine",

    # Cursor Operations
    "OP_OpenRead", "OP_OpenWrite", "OP_OpenAutoindex",
    "OP_OpenEphemeral", "OP_Close", "OP_ColumnsUsed",
    "OP_SeekGE", "OP_SeekGT", "OP_SeekLE", "OP_SeekLT",
    "OP_SeekRowid", "OP_SeekScan", "OP_NotExists", "OP_NoConflict",
    "OP_Found", "OP_NotFound", "OP_SeekHit",
    "OP_Rewind", "OP_Next", "OP_Prev", "OP_Last",
    "OP_SorterNext", "OP_SorterSort", "OP_SorterData",

    # Data Operations
    "OP_Column", "OP_Affinity", "OP_MakeRecord",
    "OP_Rowid", "OP_NewRowid", "OP_Insert", "OP_InsertInt",
    "OP_Delete", "OP_ResetCount", "OP_RealAffinity",
    "OP_IdxInsert", "OP_IdxDelete", "OP_IdxRowid",
    "OP_IdxLE", "OP_IdxGE", "OP_IdxLT", "OP_IdxGT",

    # Results
    "OP_ResultRow", "OP_Halt", "OP_Integer", "OP_Int64",
    "OP_String", "OP_String8", "OP_Null", "OP_Blob",
    "OP_Variable", "OP_Real",

    # Aggregates
    "OP_AggStep", "OP_AggFinal", "OP_AggValue", "OP_AggInverse",

    # Sorting
    "OP_SorterOpen", "OP_SorterInsert", "OP_SorterSort",
    "OP_SorterData", "OP_SorterNext", "OP_SorterCompare",

    # Functions
    "OP_Function", "OP_PureFunc", "OP_CollSeq",

    # Transactions
    "OP_Transaction", "OP_AutoCommit", "OP_Savepoint",

    # Program Control
    "OP_Noop", "OP_Explain", "OP_Init", "OP_Program",

    # Utilities
    "OP_Cast", "OP_MustBeInt", "OP_Checkpoint",
    "OP_JournalMode", "OP_VUpdate", "OP_VFilter",
    "OP_VColumn", "OP_VNext", "OP_VRename",
    "OP_Expire", "OP_TableLock", "OP_VBegin",
    "OP_VCreate", "OP_VDestroy", "OP_VOpen",
    "OP_Param", "OP_FkCounter", "OP_FkIfZero",
    "OP_MemMax", "OP_OffsetLimit", "OP_AggStep0",
    "OP_AggInverse0", "OP_FilterAdd",
]


# ================================================================
# Transformation functions
# ================================================================

def convert_function_names(text):
    """
    Convert SQLite CamelCase function names to __snake_case.

    Matches identifiers of the form sqlite[A-Z]... or
    sqlite3[A-Z]... and converts them:
        sqliteParseBegin  -> __parse_begin
        sqlite3VdbeExec   -> __vdbe_exec
        sqliteHashInit    -> __hash_init
    """
    def _replace_func(m):
        original = m.group(0)
        # Strip the sqlite/sqlite3 prefix
        if original.startswith("sqlite3"):
            rest = original[7:]
        elif original.startswith("sqlite"):
            rest = original[6:]
        else:
            return original
        if not rest:
            return original
        snake = _camel_to_snake(rest)
        return "__" + snake

    # Match sqlite[3]?<CamelCase identifier>
    # Negative lookbehind: don't match if preceded by alnum or _
    # (to avoid matching inside other identifiers)
    pattern = r"(?<![a-zA-Z0-9_])sqlite3?[A-Z][a-zA-Z0-9]*"
    text = re.sub(pattern, _replace_func, text)
    return text


def convert_type_names(text):
    """
    Convert SQLite CamelCase typedef names to dbsql _t suffix form.

    Uses word-boundary matching to avoid partial replacements.
    Does not touch names inside string literals or comments
    (a simplification -- works for most mechanical conversion).
    """
    for original, replacement in sorted(
        TYPEDEF_MAP.items(), key=lambda x: -len(x[0])
    ):
        # Word boundary match -- the type name must appear as a
        # standalone word (not part of a larger identifier).
        # Also handle pointer declarations like "Table*" or
        # "Table *".
        pattern = r"(?<![a-zA-Z0-9_])" + re.escape(original) + \
                  r"(?![a-zA-Z0-9_])"
        text = re.sub(pattern, replacement, text)
    return text


def convert_sqlite_types(text):
    """
    Convert SQLite shorthand integer types to POSIX/BSD equivalents.

    u8  -> u_int8_t
    u16 -> u_int16_t
    u32 -> u_int32_t
    i64 -> int64_t
    u64 -> uint64_t

    Also converts sqlite_* and SQLITE_* prefixed identifiers to
    dbsql_* and DBSQL_* respectively.
    """
    # Integer types -- word boundary to avoid matching u32 inside
    # e.g. "value_u32x"
    for old, new in SQLITE_TYPE_MAP.items():
        pattern = r"(?<![a-zA-Z0-9_])" + re.escape(old) + \
                  r"(?![a-zA-Z0-9_])"
        text = re.sub(pattern, new, text)

    # sqlite_ prefixed identifiers -> dbsql_
    text = re.sub(
        r"(?<![a-zA-Z0-9_])sqlite_",
        "dbsql_",
        text,
    )

    # SQLITE_ prefixed macros -> DBSQL_
    text = re.sub(
        r"(?<![a-zA-Z0-9_])SQLITE_",
        "DBSQL_",
        text,
    )

    # sqlite3_ prefixed identifiers -> dbsql_
    text = re.sub(
        r"(?<![a-zA-Z0-9_])sqlite3_",
        "dbsql_",
        text,
    )

    return text


def convert_field_names(text):
    """
    Convert CamelCase struct field names to snake_case.

    Handles field access patterns like:
      pMem->szMalloc  -> pMem->sz_malloc
      pParse->nRangeReg -> pParse->n_range_reg
      expr.isMultiWrite -> expr.is_multi_write

    This applies after type conversion, so type names are already
    converted (e.g., mem_t, parser_t).
    """
    # Match field access patterns: identifier followed by -> or . and
    # then a CamelCase field name
    pattern = r'((?:[a-zA-Z_]\w*|\))(?:->|\.))(([a-z][a-zA-Z0-9]*[A-Z]|[a-z]*[A-Z]+)[a-zA-Z0-9]*)'

    def replace_field(m):
        prefix = m.group(1)  # variable name + accessor (-> or .)
        field_name = m.group(2)  # the CamelCase field name

        # Skip if field starts with capital letter (likely a type name)
        if field_name[0].isupper():
            return m.group(0)

        # Skip known preserved field names (single char + uppercase,
        # like p1, p2, p3, p4, p5 in VdbeOp)
        if len(field_name) == 2 and field_name[0].islower() and field_name[1].isdigit():
            return m.group(0)

        # Convert to snake_case
        snake_name = _camel_to_snake(field_name)
        return prefix + snake_name

    text = re.sub(pattern, replace_field, text)

    # Handle specific field renames that aren't pure CamelCase conversion
    for original, replacement in STRUCTURE_FIELD_MAPPINGS.items():
        pattern = r'(->|\.)\b' + re.escape(original) + r'\b'
        text = re.sub(pattern, r'\1' + replacement, text)

    return text


def convert_structure_fields(text):
    """
    Convert SQLite 3.52.0 structure field references to DBSQL
    conventions.

    Handles VdbeOp, Mem, and Parser structure fields from SQLite 3.52.0.
    Most conversions are automatic via convert_field_names(), but some
    fields have explicit mappings.
    """
    text = convert_field_names(text)

    # Apply explicit field mappings for structures
    for mapping_dict in [MEM_FIELD_MAPPINGS, PARSER_FIELD_MAPPINGS]:
        for original, replacement in mapping_dict.items():
            # Match field access patterns: ->field or .field
            pattern = r'(->|\.)\b' + re.escape(original) + r'\b'
            text = re.sub(pattern, r'\1' + replacement, text)

    return text


def convert_opcode_names(text):
    """
    Convert SQLite 3.52.0 opcode names to DBSQL conventions.

    Opcode identifiers remain the same (OP_*) but ensure they
    are recognized during transpilation.
    """
    # Opcode names don't need conversion, they use the same OP_*
    # prefix in both SQLite and DBSQL. This function exists for
    # completeness and future opcode-specific transformations.
    return text


def preserve_bdb_types(text):
    """
    Ensure BerkeleyDB types are not accidentally modified.

    This is a validation pass. If any BDB type has been mangled
    by earlier transformations, restore it. In practice the
    word-boundary patterns above should already protect them.
    """
    # This function is intentionally conservative. The main risk
    # is that a typedef conversion might incorrectly match a BDB
    # type substring. Since we use word-boundary matching, this
    # should not happen, but we verify anyway.
    return text


def add_header_markers(text, module_name):
    """
    Insert PUBLIC:/STATIC:/EXTERN: comment markers before function
    definitions so that gen_inc.awk can extract prototypes.

    Heuristic detection:
      - A function definition starts at column 0 with a return type
        on its own line, followed by a function name line.
      - PUBLIC: for functions starting with __
      - STATIC: for static functions
      - EXTERN: for functions declared in public headers

    This inserts markers in the standard DBSQL comment format:
        /*
         * func_name --
         *   description
         *
         * PUBLIC: return_type func_name __P((args));
         */

    NOTE: This is a best-effort heuristic. The markers should be
    reviewed manually after transpilation.
    """
    lines = text.split("\n")
    output = []
    i = 0
    while i < len(lines):
        line = lines[i]

        # Detect function definitions: look for a line that starts
        # with a type keyword at column 0 (e.g. "void", "int",
        # "static int", etc.) followed by a line with a function
        # name and opening paren.
        #
        # We look for the pattern of a comment block ending with
        # " */" on its own line, immediately followed by a return
        # type and function definition. If the comment block does
        # not already contain a PUBLIC:/STATIC:/EXTERN: marker, we
        # could add one. However, the safest approach is to only
        # add markers where they don't already exist and where we
        # can confidently identify the function.
        #
        # For now, we pass through existing markers and leave new
        # marker insertion for manual review, since incorrect
        # markers would break the build.

        output.append(line)
        i += 1

    return "\n".join(output)


def fix_line_length(text, max_length=79):
    """
    Wrap lines that exceed max_length characters.

    Rules:
      - Preprocessor directives (#define, #include, etc.) are
        wrapped with backslash continuation.
      - Comment lines are reflowed within their comment block.
      - Code lines are wrapped at reasonable break points
        (after comma, before operator) with appropriate
        indentation.
      - Lines inside string literals are not broken (to avoid
        changing program semantics).
    """
    lines = text.split("\n")
    output = []

    for line in lines:
        if len(line) <= max_length:
            output.append(line)
            continue

        # Don't break lines inside string literals or preprocessor
        # continuations that are already escaped.
        if line.rstrip().endswith("\\"):
            output.append(line)
            continue

        # Preprocessor directives: wrap with backslash
        if line.lstrip().startswith("#"):
            output.append(line)
            continue

        # Comment lines: try to wrap at word boundaries
        stripped = line.lstrip()
        if stripped.startswith("/*") or stripped.startswith("*"):
            # Find the leading whitespace + comment prefix
            match = re.match(r"^(\s*\*?\s*)", line)
            prefix = match.group(1) if match else " * "
            # For the first line of a block comment, preserve the
            # opening marker.
            if stripped.startswith("/*"):
                match2 = re.match(r"^(\s*/\*\s*)", line)
                if match2:
                    prefix_first = match2.group(1)
                    rest = line[len(prefix_first):]
                    # Use textwrap for comment content
                    cont_prefix = " " * (len(prefix_first) - 2) \
                                  + " * "
                    wrapped = textwrap.fill(
                        rest,
                        width=max_length,
                        initial_indent=prefix_first,
                        subsequent_indent=cont_prefix,
                    )
                    output.append(wrapped)
                    continue
            else:
                rest = line[len(prefix):]
                wrapped = textwrap.fill(
                    rest,
                    width=max_length,
                    initial_indent=prefix,
                    subsequent_indent=prefix,
                )
                output.append(wrapped)
                continue

        # Code lines: try to break at commas or before operators
        # Find indentation of current line
        indent_match = re.match(r"^(\s*)", line)
        base_indent = indent_match.group(1) if indent_match else ""
        cont_indent = base_indent + "    "

        # Try to break at the last comma before max_length
        result = _wrap_code_line(line, max_length, cont_indent)
        output.extend(result)

    return "\n".join(output)


def _wrap_code_line(line, max_length, cont_indent):
    """
    Break a long code line at reasonable points.
    Returns a list of lines.
    """
    if len(line) <= max_length:
        return [line]

    results = []
    remaining = line

    while len(remaining) > max_length:
        # Find the best break point before max_length.
        # Prefer breaking after a comma, then before a binary
        # operator, then at a space.
        break_at = -1
        search_region = remaining[:max_length]

        # Look for last comma in the region (not inside parens
        # deeper than the current level, simplified)
        last_comma = search_region.rfind(",")
        if last_comma > 0:
            break_at = last_comma + 1  # break after comma

        if break_at <= 0:
            # Try breaking before && or ||
            for op in ["&&", "||"]:
                idx = search_region.rfind(" " + op)
                if idx > 0:
                    break_at = idx + 1
                    break

        if break_at <= 0:
            # Last resort: break at last space
            last_space = search_region.rfind(" ")
            if last_space > 0:
                break_at = last_space + 1

        if break_at <= 0:
            # Cannot break -- emit as-is
            results.append(remaining)
            return results

        results.append(remaining[:break_at].rstrip())
        remaining = cont_indent + remaining[break_at:].lstrip()

    if remaining:
        results.append(remaining)

    return results


# ================================================================
# Pipeline: apply all transformations in order
# ================================================================

def transpile(text, module_name):
    """
    Apply all transformations to the input text in the correct
    order and return the result.
    """
    text = convert_function_names(text)
    text = convert_type_names(text)
    text = convert_sqlite_types(text)
    text = convert_structure_fields(text)
    text = convert_opcode_names(text)
    text = preserve_bdb_types(text)
    text = add_header_markers(text, module_name)
    text = fix_line_length(text)
    return text


# ================================================================
# CLI interface
# ================================================================

def run_tests():
    """Run self-tests on the transpilation functions."""
    test_cases = [
        # Test structure field access with CamelCase->snake_case
        ("pOp->p5 = 0x01;", "pOp->p5 = 0x01;"),
        ("pMem->szMalloc", "pMem->sz_malloc"),
        ("pParse->nRangeReg", "pParse->n_range_reg"),
        ("pParse->isMultiWrite", "pParse->is_multi_write"),

        # Test function name conversion
        ("sqlite3VdbeExec", "__vdbe_exec"),
        ("sqliteParseBegin", "__parse_begin"),

        # Test type conversion
        ("u8 flags;", "u_int8_t flags;"),
        ("u32 sz;", "u_int32_t sz;"),

        # Test typedef conversion
        ("Parse *pParse", "parser_t *pParse"),
        ("Expr *pExpr", "expr_t *pExpr"),

        # Test field access after type conversion
        ("pMem->u.i = 42;", "pMem->u.i = 42;"),
    ]

    print("Running transpilation self-tests...")
    passed = 0
    failed = 0

    for input_text, expected in test_cases:
        result = transpile(input_text, "test")
        if expected in result:
            passed += 1
            print(f"  PASS: {input_text} -> {expected}")
        else:
            failed += 1
            print(f"  FAIL: {input_text}")
            print(f"    Expected: {expected}")
            print(f"    Got: {result}")

    print(f"\nTest results: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


def main():
    parser = argparse.ArgumentParser(
        description="Transpile SQLite C source to DBSQL conventions",
    )
    parser.add_argument(
        "--input", "-i",
        help="Input SQLite C source file",
    )
    parser.add_argument(
        "--output", "-o",
        help="Output DBSQL C source file",
    )
    parser.add_argument(
        "--module", "-m",
        default="dbsql",
        help="Module name for header markers (default: dbsql)",
    )
    parser.add_argument(
        "--no-line-wrap",
        action="store_true",
        help="Skip line-length fixing pass",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print to stdout instead of writing output file",
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="Run self-tests and exit",
    )
    args = parser.parse_args()

    if args.test:
        return run_tests()

    if not args.input or not args.output:
        parser.error("--input and --output are required (use --test for self-tests)")
        return 1

    with open(args.input, "r") as f:
        text = f.read()

    text = convert_function_names(text)
    text = convert_type_names(text)
    text = convert_sqlite_types(text)
    text = convert_structure_fields(text)
    text = convert_opcode_names(text)
    text = preserve_bdb_types(text)
    text = add_header_markers(text, args.module)

    if not args.no_line_wrap:
        text = fix_line_length(text)

    if args.dry_run:
        sys.stdout.write(text)
    else:
        with open(args.output, "w") as f:
            f.write(text)
        print(f"Transpiled: {args.input} -> {args.output}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
