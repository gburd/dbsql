#!/usr/bin/env python3
"""
verify_transpilation.py - Validate that transpiled code follows DBSQL conventions.

Checks:
  1. Function naming: non-static functions have __ prefix
  2. Type naming: typedefs have _t suffix
  3. BDB types preserved: DB, DBT, DBC, DB_ENV, DB_TXN not modified
  4. Header markers: PUBLIC/STATIC/EXTERN comments present
  5. Line length: lines do not exceed 79 characters
  6. ANSI C declarations: function declarations use ANSI style (not K&R)

Usage:
  ./verify_transpilation.py <file_or_directory>
  ./verify_transpilation.py --help
"""

import argparse
import os
import re
import sys


# BDB types that must never be renamed or modified during transpilation.
BDB_TYPES = frozenset({
    "DB", "DBT", "DBC", "DB_ENV", "DB_TXN", "DB_SEQUENCE",
    "DB_MPOOLFILE", "DB_LOCK", "DB_LOGC", "DB_LSN",
})

# C storage-class and type keywords that can legitimately start a line that
# looks like a function definition but is not one we need to check.
C_KEYWORDS = frozenset({
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "return", "goto", "break", "continue", "sizeof", "typedef",
    "struct", "union", "enum",
})

# Types from the standard library / BDB that are not DBSQL typedefs and
# therefore do not need the _t suffix when they appear in function signatures.
KNOWN_EXEMPT_TYPES = frozenset({
    "void", "int", "char", "short", "long", "float", "double",
    "unsigned", "signed", "size_t", "ssize_t", "off_t", "pid_t",
    "u_int8_t", "u_int16_t", "u_int32_t", "u_int64_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "db_seq_t", "db_threadid_t", "db_pgno_t", "db_recno_t",
    "time_t", "FILE", "va_list", "jmp_buf",
    "Py_ssize_t",
}) | BDB_TYPES

MAX_LINE_LENGTH = 79


class Violation:
    """A single convention violation."""

    def __init__(self, filepath, lineno, kind, message, snippet=""):
        self.filepath = filepath
        self.lineno = lineno
        self.kind = kind
        self.message = message
        self.snippet = snippet.rstrip()

    def __str__(self):
        loc = f"{self.filepath}:{self.lineno}"
        parts = [f"  {loc}: [{self.kind}] {self.message}"]
        if self.snippet:
            parts.append(f"    | {self.snippet}")
        return "\n".join(parts)


def is_c_source(path):
    """Return True if path is a C source or header file."""
    return path.endswith((".c", ".h"))


def read_lines(filepath):
    """Read file and return list of lines (preserving newlines)."""
    with open(filepath, "r", errors="replace") as f:
        return f.readlines()


# ---------------------------------------------------------------------------
# Check 1: Function naming -- non-static functions should have __ prefix
# ---------------------------------------------------------------------------

# Regex matching a C function definition at the start of a line.
# We look for lines like:
#   int\n__foo(...)          (return type on previous line)
#   __foo(type *arg, ...)    (name at start of line after return type line)
#
# The approach: find PUBLIC/STATIC marker comments and then locate the
# function definition that follows.

_MARKER_RE = re.compile(
    r"^\s*\*\s*(PUBLIC|STATIC|EXTERN):\s+"
)

# Matches a function definition line: name at start of line followed by (
# We allow optional leading type qualifiers.
_FUNC_DEF_RE = re.compile(
    r"^([a-zA-Z_]\w*)\s*\("
)

# Matches a return-type + function-name on the same line:
#   static int __foo(
#   void __bar(
_FUNC_DEF_FULL_RE = re.compile(
    r"^(?:static\s+)?(?:(?:const|unsigned|signed|volatile|struct|enum)\s+)*"
    r"(?:\w+\s*\**\s+)+(\w+)\s*\("
)


def _find_function_name_after(lines, start_idx):
    """
    Starting from start_idx, scan forward (skipping blank/comment lines)
    to find the function name in the definition.  Return (name, lineno) or
    (None, None).
    """
    i = start_idx
    while i < len(lines):
        line = lines[i].rstrip()
        # Skip blank lines and comment continuations.
        if not line or line.startswith("*") or line.startswith(" *"):
            i += 1
            continue
        # End of block comment.
        if line.strip() == "*/":
            i += 1
            continue
        # Try full definition on one line.
        m = _FUNC_DEF_FULL_RE.match(line)
        if m:
            return m.group(1), i
        # Try name-only line (return type was on previous line).
        m = _FUNC_DEF_RE.match(line)
        if m:
            name = m.group(1)
            if name not in C_KEYWORDS:
                return name, i
        # If we hit a preprocessor directive or something else, stop.
        if line.startswith("#"):
            break
        # Could be return type on its own line, continue.
        i += 1
        # But don't scan too far.
        if i - start_idx > 6:
            break
    return None, None


def check_function_naming(filepath, lines):
    """
    Check that non-static functions have __ prefix.  We locate functions
    via their PUBLIC/STATIC/EXTERN marker comments.

    Functions whose names start with ``dbsql_`` are the external public API
    entry points (e.g. dbsql_create, dbsql_version) and are exempt from the
    ``__`` prefix requirement.
    """
    violations = []
    seen_def_lines = set()  # Avoid duplicate reports from multi-line markers.
    for idx, line in enumerate(lines):
        m = _MARKER_RE.match(line)
        if not m:
            continue
        marker = m.group(1)
        # Find the function definition after the comment block.
        # The comment block ends with " */", then the definition follows.
        name, def_line = _find_function_name_after(lines, idx + 1)
        if name is None:
            continue
        if def_line in seen_def_lines:
            continue
        seen_def_lines.add(def_line)

        # External public API functions (dbsql_*) are exempt.
        if name.startswith("dbsql_"):
            continue

        # PUBLIC and EXTERN functions must have __ prefix.
        if marker in ("PUBLIC", "EXTERN"):
            if not name.startswith("__"):
                violations.append(Violation(
                    filepath, def_line + 1, "FUNC_NAMING",
                    f"Public/extern function '{name}' missing '__' prefix",
                    lines[def_line].rstrip(),
                ))
        # STATIC functions must also have __ prefix per DBSQL convention.
        elif marker == "STATIC":
            if not name.startswith("__"):
                violations.append(Violation(
                    filepath, def_line + 1, "FUNC_NAMING",
                    f"Static function '{name}' missing '__' prefix",
                    lines[def_line].rstrip(),
                ))
    return violations


# ---------------------------------------------------------------------------
# Check 2: Type naming -- typedefs should have _t suffix
# ---------------------------------------------------------------------------

_TYPEDEF_STRUCT_RE = re.compile(
    r"^\}\s*(\w+)\s*;", re.MULTILINE
)
_TYPEDEF_SIMPLE_RE = re.compile(
    r"^typedef\s+.*\s+(\w+)\s*;", re.MULTILINE
)


def check_type_naming(filepath, lines):
    """Check that typedefs end with _t suffix."""
    violations = []
    text = "".join(lines)

    # Find closing-brace typedefs:  } name_t;
    for m in _TYPEDEF_STRUCT_RE.finditer(text):
        name = m.group(1)
        # Compute line number.
        lineno = text[:m.start()].count("\n") + 1
        if not name.endswith("_t"):
            # Exclude BDB types and standard types.
            if name not in KNOWN_EXEMPT_TYPES:
                violations.append(Violation(
                    filepath, lineno, "TYPE_NAMING",
                    f"Typedef '{name}' missing '_t' suffix",
                    m.group(0).strip(),
                ))

    # Find simple typedefs: typedef <type> name_t;
    for m in _TYPEDEF_SIMPLE_RE.finditer(text):
        name = m.group(1)
        lineno = text[:m.start()].count("\n") + 1
        if not name.endswith("_t"):
            if name not in KNOWN_EXEMPT_TYPES:
                violations.append(Violation(
                    filepath, lineno, "TYPE_NAMING",
                    f"Typedef '{name}' missing '_t' suffix",
                    m.group(0).strip(),
                ))

    return violations


# ---------------------------------------------------------------------------
# Check 3: BDB types preserved
# ---------------------------------------------------------------------------

# Patterns that would indicate BDB types were renamed.  For example,
# if DB_ENV became db_env_t or dbsql_env_t, that would be wrong.
_BDB_MANGLED = {
    "DB":          re.compile(r"\bdb_t\b"),
    "DBT":         re.compile(r"\bdbt_t\b"),
    "DBC":         re.compile(r"\bdbc_t\b"),
    "DB_ENV":      re.compile(r"\bdb_env_t\b"),
    "DB_TXN":      re.compile(r"\bdb_txn_t\b"),
    "DB_SEQUENCE": re.compile(r"\bdb_sequence_t\b"),
}


def check_bdb_types(filepath, lines):
    """Check that BDB types are not mangled/renamed."""
    violations = []
    for idx, line in enumerate(lines):
        for bdb_type, pattern in _BDB_MANGLED.items():
            if pattern.search(line):
                violations.append(Violation(
                    filepath, idx + 1, "BDB_TYPE",
                    f"BDB type '{bdb_type}' appears to have been "
                    f"renamed (found mangled form)",
                    line.rstrip(),
                ))
    return violations


# ---------------------------------------------------------------------------
# Check 4: Header markers present
# ---------------------------------------------------------------------------

def check_header_markers(filepath, lines):
    """
    Check that function definitions have PUBLIC/STATIC/EXTERN marker
    comments.  We look for lines that appear to be function definitions
    (name at column 0 followed by open-paren, preceded by a return-type
    line) and verify that there is a marker comment nearby.
    """
    violations = []
    # We only check .c files, not headers.
    if not filepath.endswith(".c"):
        return violations

    # Collect line indices that have marker comments.
    marker_lines = set()
    for idx, line in enumerate(lines):
        if _MARKER_RE.match(line):
            marker_lines.add(idx)

    for idx, line in enumerate(lines):
        stripped = line.rstrip()
        # Look for function definition: name at column 0 with (
        m = _FUNC_DEF_RE.match(stripped)
        if not m:
            continue
        name = m.group(1)
        if name in C_KEYWORDS:
            continue
        # Heuristic: this is likely a function definition if the previous
        # non-blank line is a type (e.g., "static int", "void", etc.)
        # and NOT inside a function body (we check indentation == 0).
        if not stripped[0].isalpha() and not stripped[0] == '_':
            continue

        # Look backward for a return type line.
        prev_idx = idx - 1
        while prev_idx >= 0 and not lines[prev_idx].strip():
            prev_idx -= 1
        if prev_idx < 0:
            continue
        prev_line = lines[prev_idx].rstrip()
        # The return type line should be at column 0 or start with "static".
        if prev_line and (prev_line[0].isalpha() or prev_line[0] == '_'):
            # This looks like a function definition.  Check for marker.
            # Scan backward through potentially multiple comment blocks
            # (e.g. a marker comment followed by a TODO comment) looking
            # for a PUBLIC/STATIC/EXTERN marker within ~40 lines.
            has_marker = False
            scan = prev_idx - 1
            limit = max(0, idx - 40)
            while scan >= limit:
                sline = lines[scan].strip()
                if not sline:
                    scan -= 1
                    continue
                if sline == "*/":
                    # Found end of a comment block, scan inside it.
                    scan -= 1
                    while scan >= limit:
                        if scan in marker_lines:
                            has_marker = True
                            break
                        sl = lines[scan].strip()
                        if sl.startswith("/*"):
                            # Start of this block; continue scanning
                            # above it for more comment blocks.
                            scan -= 1
                            break
                        scan -= 1
                    if has_marker:
                        break
                    continue
                else:
                    # Non-blank, non-comment line -- stop scanning.
                    break
            if not has_marker and name.startswith("__"):
                violations.append(Violation(
                    filepath, idx + 1, "HEADER_MARKER",
                    f"Function '{name}' missing PUBLIC/STATIC/EXTERN "
                    f"marker comment",
                    stripped,
                ))
    return violations


# ---------------------------------------------------------------------------
# Check 5: Line length
# ---------------------------------------------------------------------------

def check_line_length(filepath, lines):
    """Check that lines do not exceed MAX_LINE_LENGTH characters."""
    violations = []
    for idx, line in enumerate(lines):
        # Strip trailing newline for length check.
        text = line.rstrip("\n").rstrip("\r")
        if len(text) > MAX_LINE_LENGTH:
            violations.append(Violation(
                filepath, idx + 1, "LINE_LENGTH",
                f"Line length {len(text)} exceeds {MAX_LINE_LENGTH}",
                text[:100] + ("..." if len(text) > 100 else ""),
            ))
    return violations


# ---------------------------------------------------------------------------
# Check 6: ANSI C declarations (not K&R)
# ---------------------------------------------------------------------------

# K&R style has the parameter declarations on separate lines after the
# function signature closing paren, like:
#
#   int
#   __foo(a, b, c)
#       int a;
#       char *b;
#       size_t c;
#   {
#
# We detect this by finding function-signature lines with only identifiers
# (no types) in the parameter list, followed by type declarations.

_KNR_PARAM_DECL_RE = re.compile(
    r"^\t(?:(?:const|unsigned|signed|volatile|static|struct|enum|union)\s+)*"
    r"(?:\w+\s*\**\s+)+\*?\w+\s*;$"
)

_PAREN_ARGS_ONLY_IDENTS_RE = re.compile(
    r"^\w+\s*\(([a-zA-Z_]\w*(?:\s*,\s*[a-zA-Z_]\w*)*)\)\s*$"
)


def check_ansi_declarations(filepath, lines):
    """
    Detect K&R-style function declarations.  K&R style is identified by
    a function signature where the parameter list contains only bare
    identifiers (no types), followed by indented parameter type declarations
    ending with semicolons before the opening brace.
    """
    violations = []
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        m = _PAREN_ARGS_ONLY_IDENTS_RE.match(line)
        if m:
            name = line.split("(")[0].strip()
            if name in C_KEYWORDS:
                i += 1
                continue
            # Check if next lines are K&R parameter declarations.
            j = i + 1
            knr_count = 0
            while j < len(lines):
                next_line = lines[j].rstrip()
                if _KNR_PARAM_DECL_RE.match(next_line):
                    knr_count += 1
                    j += 1
                else:
                    break
            if knr_count > 0:
                # Check that the line after declarations is {
                if j < len(lines) and lines[j].strip() == "{":
                    violations.append(Violation(
                        filepath, i + 1, "KNR_DECL",
                        f"K&R-style function declaration for '{name}' "
                        f"(use ANSI C style)",
                        line,
                    ))
                    i = j + 1
                    continue
        i += 1
    return violations


# ---------------------------------------------------------------------------
# Check 7: VDBE opcode validation (SQLite 3.52.0)
# ---------------------------------------------------------------------------

def validate_vdbe_opcodes(filepath, lines):
    """Validate VdbeOp structure initialization patterns."""
    violations = []
    text = "".join(lines)

    # Check P5 flag initialization
    p5_pattern = r'\.p5\s*=\s*(?P<value>0x[0-9a-fA-F]+|\d+)'
    for match in re.finditer(p5_pattern, text):
        value = match.group('value')
        lineno = text[:match.start()].count("\n") + 1
        # P5 should be u16, check range
        int_val = int(value, 0)
        if int_val > 0xFFFF:
            violations.append(Violation(
                filepath, lineno, "VDBE_P5",
                f"P5 value out of range (u16): {value}",
                lines[lineno - 1].rstrip(),
            ))

    # Validate register number ranges (0-255 typical, up to 32767 max)
    reg_pattern = r'(?:p1|p2|p3)\s*=\s*(?P<reg>\d+)'
    for match in re.finditer(reg_pattern, text):
        reg_str = match.group('reg')
        reg = int(reg_str)
        lineno = text[:match.start()].count("\n") + 1
        if reg > 32767:
            violations.append(Violation(
                filepath, lineno, "VDBE_REG",
                f"Register number exceeds maximum (32767): {reg}",
                lines[lineno - 1].rstrip(),
            ))
        elif reg > 255:
            # Warning for unusually high register numbers
            violations.append(Violation(
                filepath, lineno, "VDBE_REG_HIGH",
                f"Register number unusually high (>255): {reg}",
                lines[lineno - 1].rstrip(),
            ))

    # Check P4 type consistency
    p4type_pattern = r'\.p4type\s*=\s*(?P<type>P4_\w+)'
    for match in re.finditer(p4type_pattern, text):
        p4type = match.group('type')
        lineno = text[:match.start()].count("\n") + 1
        # Validate p4type is a known type
        known_types = ['P4_NOTUSED', 'P4_INT32', 'P4_INT64', 'P4_REAL',
                      'P4_STRING', 'P4_KEYINFO', 'P4_COLLSEQ', 'P4_FUNCDEF',
                      'P4_INTARRAY', 'P4_SUBPROGRAM', 'P4_POINTER']
        if p4type not in known_types:
            violations.append(Violation(
                filepath, lineno, "VDBE_P4TYPE",
                f"Unknown P4 type: {p4type}",
                lines[lineno - 1].rstrip(),
            ))

    return violations


def validate_mem_structure(filepath, lines):
    """Validate Mem (register) initialization patterns."""
    violations = []
    text = "".join(lines)

    # Check for invalid MEM_* flag combinations
    flag_pattern = r'(?P<var>\w+)->flags\s*=\s*(?P<flags>MEM_\w+(?:\s*\|\s*MEM_\w+)*)'
    for match in re.finditer(flag_pattern, text):
        flags_str = match.group('flags')
        lineno = text[:match.start()].count("\n") + 1

        # Extract individual flags
        flags = [f.strip() for f in flags_str.split('|')]

        # Type flags are mutually exclusive
        type_flags = ['MEM_Int', 'MEM_Real', 'MEM_Str', 'MEM_Blob', 'MEM_Null']
        type_count = sum(1 for f in flags if f in type_flags)

        if type_count > 1:
            violations.append(Violation(
                filepath, lineno, "MEM_TYPE_FLAGS",
                f"Multiple type flags (should be exclusive): {flags_str}",
                lines[lineno - 1].rstrip(),
            ))

        # Check for invalid combinations
        if 'MEM_Str' in flags and 'MEM_Blob' in flags:
            violations.append(Violation(
                filepath, lineno, "MEM_FLAGS",
                f"Invalid MEM flag combination (Str+Blob): {flags_str}",
                lines[lineno - 1].rstrip(),
            ))

    # Check for proper cleanup patterns (xDel callback)
    xdel_pattern = r'(?P<var>\w+)->x_del\s*=\s*(?P<func>\w+)'
    for match in re.finditer(xdel_pattern, text):
        func = match.group('func')
        lineno = text[:match.start()].count("\n") + 1
        # Verify MEM_Dyn flag is also set nearby
        # This is a best-effort check
        context_start = max(0, match.start() - 200)
        context_end = min(len(text), match.end() + 200)
        context = text[context_start:context_end]
        if 'MEM_Dyn' not in context and func != 'NULL' and func != '0':
            violations.append(Violation(
                filepath, lineno, "MEM_XDL",
                f"xDel set to {func} but MEM_Dyn flag not found in context",
                lines[lineno - 1].rstrip(),
            ))

    # Check szMalloc/zMalloc consistency
    szmalloc_pattern = r'(?P<var>\w+)->sz_malloc\s*=\s*(?P<size>\d+)'
    for match in re.finditer(szmalloc_pattern, text):
        var = match.group('var')
        size = match.group('size')
        lineno = text[:match.start()].count("\n") + 1
        # Check if zMalloc is also set nearby
        context_start = max(0, match.start() - 200)
        context_end = min(len(text), match.end() + 200)
        context = text[context_start:context_end]
        if f'{var}->z_malloc' not in context and int(size) > 0:
            violations.append(Violation(
                filepath, lineno, "MEM_MALLOC",
                f"sz_malloc set to {size} but z_malloc not found in context",
                lines[lineno - 1].rstrip(),
            ))

    return violations


def validate_register_addressing(filepath, lines):
    """Validate register addressing is 0-indexed."""
    violations = []
    text = "".join(lines)

    # Check for 1-indexed patterns (common mistake)
    suspicious_patterns = [
        (r'(?:reg|iReg)\s*=\s*1;\s*//.*(?:first|start)', "Register starts at 1"),
        (r'(?:reg|iReg)\s*=\s*0;\s*//.*(?:skip|unused)', "Register 0 marked as unused"),
    ]

    for pattern, desc in suspicious_patterns:
        for match in re.finditer(pattern, text):
            lineno = text[:match.start()].count("\n") + 1
            violations.append(Violation(
                filepath, lineno, "REG_ADDR",
                f"Suspicious register pattern: {desc}",
                lines[lineno - 1].rstrip(),
            ))

    return violations


def check_vdbe_code(filepath, lines):
    """Main validation entry for VDBE-related files."""
    violations = []
    filename = os.path.basename(filepath)

    # Run VDBE checks on vdbe.c and code generators
    if 'vdbe' in filename.lower():
        violations.extend(validate_vdbe_opcodes(filepath, lines))
        violations.extend(validate_mem_structure(filepath, lines))
        violations.extend(validate_register_addressing(filepath, lines))

    if filename.startswith('cg_'):
        violations.extend(validate_register_addressing(filepath, lines))

    return violations


# ---------------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------------

ALL_CHECKS = [
    ("Function naming", check_function_naming),
    ("Type naming", check_type_naming),
    ("BDB types preserved", check_bdb_types),
    ("Header markers", check_header_markers),
    ("Line length", check_line_length),
    ("ANSI C declarations", check_ansi_declarations),
    ("VDBE validation", check_vdbe_code),
]


def check_file(filepath):
    """Run all checks on a single file.  Return list of Violations."""
    lines = read_lines(filepath)
    violations = []
    for _name, check_fn in ALL_CHECKS:
        violations.extend(check_fn(filepath, lines))
    return violations


def collect_files(path):
    """Collect C source files from a path (file or directory)."""
    if os.path.isfile(path):
        if is_c_source(path):
            return [path]
        return []
    files = []
    for root, _dirs, filenames in os.walk(path):
        # Skip hidden directories and known non-source trees.
        basename = os.path.basename(root)
        if basename.startswith(".") or basename in ("_",):
            continue
        for fn in sorted(filenames):
            fp = os.path.join(root, fn)
            if is_c_source(fp):
                files.append(fp)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(
        description="Verify transpiled C code follows DBSQL conventions.",
    )
    parser.add_argument(
        "path",
        help="File or directory to check",
    )
    parser.add_argument(
        "--no-line-length", action="store_true",
        help="Disable line-length checking",
    )
    parser.add_argument(
        "--no-knr", action="store_true",
        help="Disable K&R declaration checking",
    )
    parser.add_argument(
        "--no-markers", action="store_true",
        help="Disable header marker checking",
    )
    parser.add_argument(
        "--no-vdbe", action="store_true",
        help="Disable VDBE validation checking",
    )
    parser.add_argument(
        "--summary-only", action="store_true",
        help="Only print summary, not individual violations",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Print file names as they are checked",
    )
    args = parser.parse_args()

    # Adjust checks based on flags.
    global ALL_CHECKS
    if args.no_line_length:
        ALL_CHECKS = [(n, f) for n, f in ALL_CHECKS if n != "Line length"]
    if args.no_knr:
        ALL_CHECKS = [(n, f) for n, f in ALL_CHECKS
                       if n != "ANSI C declarations"]
    if args.no_markers:
        ALL_CHECKS = [(n, f) for n, f in ALL_CHECKS
                       if n != "Header markers"]
    if args.no_vdbe:
        ALL_CHECKS = [(n, f) for n, f in ALL_CHECKS
                       if n != "VDBE validation"]

    files = collect_files(args.path)
    if not files:
        print(f"No C source files found at: {args.path}", file=sys.stderr)
        sys.exit(1)

    total_files = len(files)
    passed_files = 0
    all_violations = {}

    for filepath in files:
        if args.verbose:
            print(f"Checking: {filepath}", file=sys.stderr)
        violations = check_file(filepath)
        if violations:
            all_violations[filepath] = violations
        else:
            passed_files += 1

    # Print results.
    print("=" * 60)
    print("DBSQL Transpilation Verification Report")
    print("=" * 60)
    print()

    if all_violations and not args.summary_only:
        # Group violations by type for easier reading.
        by_kind = {}
        for filepath, viols in sorted(all_violations.items()):
            for v in viols:
                by_kind.setdefault(v.kind, []).append(v)

        for kind in sorted(by_kind.keys()):
            viols = by_kind[kind]
            print(f"--- {kind} ({len(viols)} violations) ---")
            for v in viols:
                print(v)
            print()

    # Summary.
    failed_files = total_files - passed_files
    total_violations = sum(len(v) for v in all_violations.values())

    print("-" * 60)
    print(f"Files checked:  {total_files}")
    print(f"Files passed:   {passed_files}/{total_files}")
    print(f"Files failed:   {failed_files}/{total_files}")
    print(f"Total violations: {total_violations}")

    if all_violations:
        # Per-kind summary.
        by_kind = {}
        for viols in all_violations.values():
            for v in viols:
                by_kind[v.kind] = by_kind.get(v.kind, 0) + 1
        print()
        print("Violation breakdown:")
        for kind, count in sorted(by_kind.items()):
            print(f"  {kind}: {count}")

    print("-" * 60)

    if failed_files > 0:
        print(f"FAIL: {failed_files} file(s) have violations")
        sys.exit(1)
    else:
        print("PASS: All files conform to DBSQL conventions")
        sys.exit(0)


if __name__ == "__main__":
    main()
